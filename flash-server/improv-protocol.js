const HEADER = new TextEncoder().encode('IMPROV')
const VERSION = 1
export const RPC_PROTO_VERSION = 3
const TYPE_RPC_COMMAND = 0x03
const TYPE_RPC_RESULT = 0x04
const TYPE_ERROR_STATE = 0x02

export const Commands = {
  LIST_CONFIG: 0x40,
  SET_NETWORK: 0x41,
  DELETE_NETWORK: 0x42,
  REORDER_NETWORKS: 0x43,
  REORDER_APIS: 0x44,
  REPROVISION_WIFI: 0x45,
  LIST_CONFIG_NETWORK: 0x46,
  LIST_CONFIG_API: 0x47,
  LIST_CONFIG_FIELD: 0x48,
  SET_NETWORK_BEGIN: 0x49,
  SET_NETWORK_API: 0x4a,
  SET_NETWORK_FIELD: 0x4b,
  SET_NETWORK_COMMIT: 0x4c,
}

const encoder = new TextEncoder()
const decoder = new TextDecoder()

function checksum(bytes) {
  return bytes.reduce((sum, byte) => (sum + byte) & 0xff, 0)
}

function encodeFrame(type, data) {
  if (data.length > 255) throw new Error('Improv payload exceeds 255 bytes')
  const frame = new Uint8Array(HEADER.length + 3 + data.length + 1)
  frame.set(HEADER, 0)
  frame[6] = VERSION
  frame[7] = type
  frame[8] = data.length
  frame.set(data, 9)
  frame[frame.length - 1] = checksum([...frame.slice(6, frame.length - 1)])
  return frame
}

export function encodeRpc(command, payload = {}) {
  const json = encoder.encode(JSON.stringify({ proto_version: RPC_PROTO_VERSION, ...payload }))
  if (json.length > 253) throw new Error('RPC JSON payload exceeds 253 bytes')
  const data = new Uint8Array(json.length + 2)
  data[0] = command
  data[1] = json.length
  data.set(json, 2)
  return encodeFrame(TYPE_RPC_COMMAND, data)
}

export function tryDecodeFrames(buffer) {
  const frames = []
  let offset = 0

  while (buffer.length - offset >= 10) {
    if (decoder.decode(buffer.slice(offset, offset + 6)) !== 'IMPROV') {
      offset += 1
      continue
    }
    const version = buffer[offset + 6]
    const type = buffer[offset + 7]
    const length = buffer[offset + 8]
    const frameEnd = offset + 10 + length
    if (buffer.length < frameEnd) break
    const data = buffer.slice(offset + 9, offset + 9 + length)
    const expected = buffer[offset + 9 + length]
    const actual = checksum([version, type, length, ...data])
    if (version === VERSION && expected === actual) frames.push({ type, data })
    offset = frameEnd
  }

  return { frames, remaining: buffer.slice(offset) }
}

export class ImprovSerial {
  constructor() {
    this.port = null
    this.reader = null
    this.writer = null
    this.readLoopPromise = null
    this.buffer = new Uint8Array()
    this.waiters = []
    /* Set by the UI to react to the device going away (USB re-enumeration
     * after a chip reset, cable unplug, browser tab losing the port). Called
     * at most once per connect/disconnect cycle with a reason string. */
    this.onDisconnect = null
    this._serialDisconnectHandler = null
    this._disconnecting = false
  }

  get connected() {
    return this.port !== null
  }

  async connect() {
    if (this.port) await this.disconnect('Serial device is reconnecting')
    this.port = await navigator.serial.requestPort()
    try {
      /* Match esp-web-tools: open with only baudRate and leave modem-control
       * signals alone. Some hosts reset ESP32-S3 USB-Serial-JTAG when the CDC
       * port opens; the ready probe below waits through that reset before the
       * UI is allowed to send config RPCs. */
      await this.port.open({ baudRate: 115200 })
      this.writer = this.port.writable.getWriter()
      this.reader = this.port.readable.getReader()
      this.readLoopPromise = this.readLoop(this.reader)

      /* navigator.serial fires 'disconnect' when the underlying USB device
       * goes away (chip soft-reset on ESP32-S3 re-enumerates as a new device,
       * cable unplug, etc.). Without this hook the UI keeps writing to a port
       * whose handle is stale and RPCs silently time out. */
      this._serialDisconnectHandler = (event) => {
        if (event.target === this.port) {
          this._notifyDisconnected('Device disconnected (USB re-enumeration or unplug)')
        }
      }
      navigator.serial.addEventListener('disconnect', this._serialDisconnectHandler)
      await this.waitUntilReady()
    } catch (err) {
      await this.disconnect(err)
      throw err
    }
  }

  async waitUntilReady(timeoutMs = 45000) {
    const deadline = Date.now() + timeoutMs
    let lastError = null
    while (Date.now() < deadline && this.writer) {
      try {
        await this.rpc(Commands.LIST_CONFIG, {}, { timeoutMs: 1500 })
        return
      } catch (err) {
        lastError = err
        if (!this.writer) break
        await new Promise((resolve) => setTimeout(resolve, 500))
      }
    }
    throw new Error(`Improv did not become ready${lastError ? `: ${lastError.message}` : ''}`)
  }

  async _notifyDisconnected(reason) {
    if (!this.port && !this.reader) return
    const callback = this.onDisconnect
    /* Tear down state first so a UI handler that checks `serial.connected`
     * sees the post-disconnect state. */
    await this.disconnect(reason).catch(() => {})
    if (callback) {
      try {
        callback(reason)
      } catch {
        // Swallow UI handler errors — they must not break the teardown path.
      }
    }
  }

  async readLoop(reader) {
    try {
      while (this.reader === reader) {
        const { value, done } = await reader.read()
        if (done) break
        if (!value) continue
        const next = new Uint8Array(this.buffer.length + value.length)
        next.set(this.buffer)
        next.set(value, this.buffer.length)
        const decoded = tryDecodeFrames(next)
        this.buffer = decoded.remaining
        for (const frame of decoded.frames) this.resolveFrame(frame)
      }
    } catch (err) {
      if (this.reader === reader) {
        this.rejectWaiters(new Error(`Serial read failed: ${err.message}`))
      }
    } finally {
      try {
        reader.releaseLock()
      } catch {
        // The lock may already be released while closing after a USB reset.
      }
      if (this.reader === reader) {
        this.reader = null
        this.readLoopPromise = null
        if (!this._disconnecting) {
          this._notifyDisconnected('Serial stream closed').catch(() => {})
        }
      }
    }
  }

  rejectWaiters(error) {
    const waiters = this.waiters.splice(0)
    for (const waiter of waiters) {
      clearTimeout(waiter.timer)
      waiter.reject(error)
    }
  }

  async disconnect(reason = 'Serial device disconnected') {
    const error = reason instanceof Error ? reason : new Error(reason)
    const reader = this.reader
    const readLoopPromise = this.readLoopPromise
    const writer = this.writer
    const port = this.port

    this._disconnecting = true
    this.reader = null
    this.readLoopPromise = null
    this.writer = null
    this.port = null
    this.buffer = new Uint8Array()
    this.rejectWaiters(error)

    if (this._serialDisconnectHandler) {
      navigator.serial.removeEventListener('disconnect', this._serialDisconnectHandler)
      this._serialDisconnectHandler = null
    }

    if (reader) {
      try {
        await reader.cancel(error.message)
      } catch {
        // The device may already have reset and closed the stream.
      }
    }

    if (readLoopPromise) await readLoopPromise.catch(() => {})

    if (writer) {
      try {
        await writer.close()
      } catch {
        // Closing can fail if the device reset first; releasing the lock still matters.
      } finally {
        try {
          writer.releaseLock()
        } catch {
          // Already released.
        }
      }
    }

    if (port) {
      try {
        await port.close()
      } catch {
        // The browser may already consider the device gone after USB re-enumeration.
      }
    }
    this._disconnecting = false
  }

  resolveFrame(frame) {
    if (frame.type === TYPE_ERROR_STATE) {
      const waiter = this.waiters.shift()
      if (waiter) {
        clearTimeout(waiter.timer)
        waiter.reject(new Error(`Device returned Improv error ${frame.data[0]}`))
      }
      return
    }
    if (frame.type !== TYPE_RPC_RESULT) return
    const command = frame.data[0]
    const length = frame.data[1] ?? 0
    const json = decoder.decode(frame.data.slice(2, 2 + length))
    const waiterIndex = this.waiters.findIndex((waiter) => waiter.command === command)
    if (waiterIndex < 0) return
    const [waiter] = this.waiters.splice(waiterIndex, 1)
    clearTimeout(waiter.timer)
    try {
      waiter.resolve(json ? JSON.parse(json) : {})
    } catch (err) {
      waiter.reject(err)
    }
  }

  async rpc(command, payload = {}, options = {}) {
    if (!this.writer) throw new Error('Serial device is not connected')
    const timeoutMs = options.timeoutMs ?? 15000
    let timer
    let waiter
    const response = new Promise((resolve, reject) => {
      waiter = { command, resolve, reject }
      this.waiters.push(waiter)
      timer = setTimeout(() => {
        const idx = this.waiters.indexOf(waiter)
        if (idx >= 0) this.waiters.splice(idx, 1)
        reject(new Error('RPC timed out'))
      }, timeoutMs)
      waiter.timer = timer
    })
    const write = this.writer.write(encodeRpc(command, payload))
    write.catch(() => {})
    try {
      await Promise.race([write, response])
      return await response
    } finally {
      const idx = this.waiters.indexOf(waiter)
      if (idx >= 0) this.waiters.splice(idx, 1)
      clearTimeout(timer)
    }
  }
}
