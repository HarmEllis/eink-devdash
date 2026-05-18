const HEADER = new TextEncoder().encode('IMPROV')
const VERSION = 1
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
  const json = encoder.encode(JSON.stringify({ proto_version: 2, ...payload }))
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
    this.buffer = new Uint8Array()
    this.waiters = []
  }

  async connect() {
    this.port = await navigator.serial.requestPort()
    await this.port.open({ baudRate: 115200 })
    this.writer = this.port.writable.getWriter()
    this.reader = this.port.readable.getReader()
    this.readLoop()
  }

  async readLoop() {
    while (this.reader) {
      const { value, done } = await this.reader.read()
      if (done) break
      if (!value) continue
      const next = new Uint8Array(this.buffer.length + value.length)
      next.set(this.buffer)
      next.set(value, this.buffer.length)
      const decoded = tryDecodeFrames(next)
      this.buffer = decoded.remaining
      for (const frame of decoded.frames) this.resolveFrame(frame)
    }
  }

  resolveFrame(frame) {
    if (frame.type === TYPE_ERROR_STATE) {
      const waiter = this.waiters.shift()
      waiter?.reject(new Error(`Device returned Improv error ${frame.data[0]}`))
      return
    }
    if (frame.type !== TYPE_RPC_RESULT) return
    const command = frame.data[0]
    const length = frame.data[1] ?? 0
    const json = decoder.decode(frame.data.slice(2, 2 + length))
    const waiterIndex = this.waiters.findIndex((waiter) => waiter.command === command)
    if (waiterIndex < 0) return
    const [waiter] = this.waiters.splice(waiterIndex, 1)
    try {
      waiter.resolve(json ? JSON.parse(json) : {})
    } catch (err) {
      waiter.reject(err)
    }
  }

  async rpc(command, payload = {}) {
    if (!this.writer) throw new Error('Serial device is not connected')
    const response = new Promise((resolve, reject) => {
      this.waiters.push({ command, resolve, reject })
      setTimeout(() => reject(new Error('RPC timed out')), 15000)
    })
    await this.writer.write(encodeRpc(command, payload))
    return response
  }
}
