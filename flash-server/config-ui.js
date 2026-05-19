import { Commands, ImprovSerial, RPC_PROTO_VERSION } from './improv-protocol.js'

const FIELD_CHUNK_SIZE = 120
const MAX_WIFI_NETWORKS = 5
const MAX_APIS_PER_NETWORK = 5
const DASH_API_URL_MAX = 192
const RESTART_GRACE_MS = 3000
let nextClientId = 1

const serial = new ImprovSerial()
const state = {
  config: { proto_version: RPC_PROTO_VERSION, refresh_min: 5, networks: [] },
  selectedNetworkKey: null,
}

const statusEl = document.querySelector('#status')
const feedbackEl = document.querySelector('#action-feedback')
const connectButton = document.querySelector('#connect-button')
const disconnectButton = document.querySelector('#disconnect-button')
const loadButton = document.querySelector('#load-button')
const reprovisionButton = document.querySelector('#reprovision-button')
const addNetworkButton = document.querySelector('#add-network-button')
const saveButton = document.querySelector('#save-button')
const reorderNetworksButton = document.querySelector('#reorder-networks-button')
const networkList = document.querySelector('#network-list')

function setStatus(message) {
  statusEl.textContent = message
}

function setFeedback(kind, message) {
  feedbackEl.textContent = message
  feedbackEl.className = `action-feedback ${kind}`
}

function clearFeedback() {
  feedbackEl.textContent = ''
  feedbackEl.className = 'action-feedback'
}

function enableControls(enabled) {
  loadButton.disabled = !enabled
  reprovisionButton.disabled = !enabled
  addNetworkButton.disabled = !enabled
  saveButton.disabled = !enabled
  reorderNetworksButton.disabled = !enabled
  disconnectButton.disabled = !enabled
}

function applyDisconnectedUi(message) {
  enableControls(false)
  connectButton.disabled = false
  connectButton.textContent = 'Reconnect Serial'
  setStatus('Disconnected')
  if (message) setFeedback('busy', message)
}

function delay(ms) {
  return new Promise((resolve) => {
    setTimeout(resolve, ms)
  })
}

async function waitForRestart(statusMessage, reconnectMessage) {
  setStatus(statusMessage)
  setFeedback('busy', 'Device is restarting — please wait...')
  enableControls(false)
  connectButton.disabled = true
  await serial.disconnect('device restarting')
  await delay(RESTART_GRACE_MS)
  connectButton.textContent = 'Reconnect Serial'
  connectButton.disabled = false
  setFeedback('busy', reconnectMessage)
}

function newClientKey(prefix) {
  return `${prefix}-${nextClientId++}`
}

function normalizeNetwork(network) {
  network._clientKey ??= newClientKey('network')
  network.apis ??= []
  for (const api of network.apis) api._clientKey ??= newClientKey('api')
  return network
}

function selectedNetwork() {
  return state.config.networks.find((network) => network._clientKey === state.selectedNetworkKey)
}

function moveItem(items, index, delta) {
  const next = index + delta
  if (next < 0 || next >= items.length) return
  const [item] = items.splice(index, 1)
  items.splice(next, 0, item)
  render()
}

function selectNetwork(key) {
  state.selectedNetworkKey = key
  for (const card of networkList.querySelectorAll('.network')) {
    card.classList.toggle('selected', card.dataset.networkKey === key)
  }
}

function activeInputSnapshot() {
  const active = document.activeElement
  if (!(active instanceof HTMLInputElement) || !networkList.contains(active)) return null
  return {
    key: active.dataset.focusKey,
    selectionStart: active.selectionStart,
    selectionEnd: active.selectionEnd,
  }
}

function restoreInputFocus(snapshot) {
  if (!snapshot?.key) return
  const input = [...networkList.querySelectorAll('input')].find(
    (candidate) => candidate.dataset.focusKey === snapshot.key,
  )
  if (!input) return
  input.focus()
  if (typeof snapshot.selectionStart === 'number' && typeof snapshot.selectionEnd === 'number') {
    input.setSelectionRange(snapshot.selectionStart, snapshot.selectionEnd)
  }
}

function render() {
  const focusSnapshot = activeInputSnapshot()
  networkList.replaceChildren()
  for (const [networkIndex, network] of state.config.networks.entries()) {
    normalizeNetwork(network)
    const card = document.createElement('article')
    card.className = `network ${network._clientKey === state.selectedNetworkKey ? 'selected' : ''}`
    card.dataset.networkKey = network._clientKey
    card.addEventListener('focusin', () => selectNetwork(network._clientKey))

    const header = document.createElement('div')
    header.className = 'network-header'
    const title = document.createElement('strong')
    title.textContent = network.ssid || 'New network'
    header.append(title)
    const controls = document.createElement('div')
    controls.className = 'row'
    controls.append(
      button('Up', () => moveItem(state.config.networks, networkIndex, -1)),
      button('Down', () => moveItem(state.config.networks, networkIndex, 1)),
      button('Delete', () => deleteNetwork(network)),
    )
    header.append(controls)
    card.append(header)

    const grid = document.createElement('div')
    grid.className = 'grid'
    grid.append(
      inputField('SSID', network.ssid, (value) => {
        network.ssid = value
        title.textContent = value || 'New network'
      }, 'text', `${network._clientKey}:ssid`),
      inputField('Password', network.password ?? '', (value) => {
        network.password = value
      }, 'password', `${network._clientKey}:password`),
    )
    card.append(grid)

    const apiHeader = document.createElement('div')
    apiHeader.className = 'api-header'
    apiHeader.innerHTML = '<strong>APIs</strong>'
    const apiHeaderControls = document.createElement('div')
    apiHeaderControls.className = 'row'
    apiHeaderControls.append(button('Apply API Order', () => reorderApis(network)))
    apiHeaderControls.append(button('Add API', () => {
      if (network.apis.length >= MAX_APIS_PER_NETWORK) {
        setFeedback('error', `A network can have at most ${MAX_APIS_PER_NETWORK} APIs.`)
        return
      }
      network.apis.push({
        id: 0,
        _clientKey: newClientKey('api'),
        enabled: true,
        api_url: 'http://',
        device_token: '',
      })
      render()
    }))
    apiHeader.append(apiHeaderControls)
    card.append(apiHeader)

    for (const [apiIndex, api] of network.apis.entries()) {
      api._clientKey ??= newClientKey('api')
      const row = document.createElement('div')
      row.className = 'api api-grid'
      row.append(
        inputField('API URL', api.api_url, (value) => { api.api_url = value }, 'url', `${api._clientKey}:api_url`),
        inputField('Token', api.device_token ?? '', (value) => { api.device_token = value }, 'password', `${api._clientKey}:device_token`),
        apiControls(network, apiIndex),
      )
      card.append(row)
    }
    networkList.append(card)
  }
  restoreInputFocus(focusSnapshot)
}

function button(label, onClick) {
  const el = document.createElement('button')
  el.type = 'button'
  el.textContent = label
  el.addEventListener('click', () => onClick())
  return el
}

function inputField(label, value, onInput, type = 'text', focusKey = '') {
  const wrapper = document.createElement('label')
  wrapper.textContent = label
  const input = document.createElement('input')
  input.type = type
  input.value = value ?? ''
  input.autocomplete = 'off'
  input.dataset.focusKey = focusKey
  input.addEventListener('input', () => onInput(input.value))
  wrapper.append(input)
  return wrapper
}

function apiControls(network, apiIndex) {
  const controls = document.createElement('div')
  controls.className = 'row'
  controls.append(
    button('Up', () => moveItem(network.apis, apiIndex, -1)),
    button('Down', () => moveItem(network.apis, apiIndex, 1)),
    button('Delete', () => {
      network.apis.splice(apiIndex, 1)
      render()
    }),
  )
  return controls
}

async function readConfigField(networkIndex, apiIndex, field) {
  let value = ''
  let offset = 0
  for (;;) {
    const chunk = await serial.rpc(Commands.LIST_CONFIG_FIELD, {
      network_index: networkIndex,
      api_index: apiIndex,
      field,
      offset,
    })
    const part = chunk.value ?? ''
    value += part
    offset += part.length
    if (chunk.done) return value
    if (part.length === 0) throw new Error(`Device returned an empty ${field} chunk`)
  }
}

async function loadConfig() {
  setStatus('Loading config...')
  setFeedback('busy', 'Reading config from device...')
  const summary = await serial.rpc(Commands.LIST_CONFIG)
  const networks = []
  const networkCount = summary.network_count ?? 0
  for (let networkIndex = 0; networkIndex < networkCount; networkIndex += 1) {
    const network = normalizeNetwork(await serial.rpc(Commands.LIST_CONFIG_NETWORK, { network_index: networkIndex }))
    network.password = ''
    network.apis = []
    for (let apiIndex = 0; apiIndex < (network.api_count ?? 0); apiIndex += 1) {
      const api = await serial.rpc(Commands.LIST_CONFIG_API, { network_index: networkIndex, api_index: apiIndex })
      api._clientKey = newClientKey('api')
      api.api_url = await readConfigField(networkIndex, apiIndex, 'api_url')
      api.device_token = await readConfigField(networkIndex, apiIndex, 'device_token')
      network.apis.push(api)
    }
    delete network.api_count
    networks.push(network)
  }
  state.config = { ...summary, networks }
  state.selectedNetworkKey = networks[0]?._clientKey ?? null
  render()
  setStatus('Config loaded')
  setFeedback('success', `Loaded ${networks.length} network(s).`)
}

function validateApiUrl(url) {
  if (!url) return 'API URL is required'
  if (!url.startsWith('http://')) return 'API URLs must start with http://'
  if (url.length >= DASH_API_URL_MAX) return `API URLs must be shorter than ${DASH_API_URL_MAX} characters`
  const host = url.slice('http://'.length)
  if (!host) return 'API URLs must include a host after http://'
  if (/[^A-Za-z0-9.\-:/_]/.test(host)) {
    return 'API URLs may only contain letters, numbers, dots, dashes, colons, slashes, and underscores after http://'
  }
  return null
}

function validateNetwork(network) {
  if (!network.ssid) throw new Error('SSID is required')
  if (state.config.networks.length > MAX_WIFI_NETWORKS) {
    throw new Error(`A config can have at most ${MAX_WIFI_NETWORKS} networks`)
  }
  if (network.apis.length > MAX_APIS_PER_NETWORK) {
    throw new Error(`A network can have at most ${MAX_APIS_PER_NETWORK} APIs`)
  }
  for (const api of network.apis) {
    const error = validateApiUrl(api.api_url)
    if (error) throw new Error(error)
  }
}

async function writeConfigField(networkId, apiIndex, field, value) {
  for (let offset = 0; offset < value.length || offset === 0; offset += FIELD_CHUNK_SIZE) {
    const chunk = value.slice(offset, offset + FIELD_CHUNK_SIZE)
    const done = offset + FIELD_CHUNK_SIZE >= value.length
    await serial.rpc(Commands.SET_NETWORK_FIELD, {
      network_id: networkId,
      api_index: apiIndex,
      field,
      offset,
      value: chunk,
      done,
    })
    if (done) return
  }
}

async function saveSelectedNetwork() {
  const network = selectedNetwork()
  if (!network) throw new Error('Click into a network card first to highlight it for saving')
  validateNetwork(network)
  setStatus('Saving network...')
  setFeedback('busy', `Saving "${network.ssid}"...`)

  const begin = await serial.rpc(Commands.SET_NETWORK_BEGIN, {
    id: network.id || 0,
    enabled: network.enabled !== false,
    ssid: network.ssid,
    password: network.password ?? '',
    api_count: network.apis.length,
  })
  const networkId = begin.id
  for (const [apiIndex, api] of network.apis.entries()) {
    const savedApi = await serial.rpc(Commands.SET_NETWORK_API, {
      network_id: networkId,
      api_index: apiIndex,
      id: api.id || 0,
      enabled: api.enabled !== false,
    })
    api.id = savedApi.id
    await writeConfigField(networkId, apiIndex, 'api_url', api.api_url)
    if (api.device_token && !api.device_token.startsWith('****')) {
      await writeConfigField(networkId, apiIndex, 'device_token', api.device_token)
    }
  }
  await serial.rpc(Commands.SET_NETWORK_COMMIT, { network_id: networkId })
  await waitForRestart('Saved; device is restarting', `Saved "${network.ssid}". Reconnect Serial to continue.`)
}

async function deleteNetwork(network) {
  if (!network.id) {
    state.config.networks = state.config.networks.filter((item) => item !== network)
    if (state.selectedNetworkKey === network._clientKey) {
      state.selectedNetworkKey = state.config.networks[0]?._clientKey ?? null
    }
    render()
    return
  }
  setStatus('Deleting network...')
  setFeedback('busy', `Deleting "${network.ssid}"...`)
  await serial.rpc(Commands.DELETE_NETWORK, { id: network.id })
  await waitForRestart('Deleted; device is restarting', `Deleted "${network.ssid}". Reconnect Serial to continue.`)
}

async function reorderApis(network) {
  if (!network.id) throw new Error('Save this network before applying API order')
  if (network.apis.some((api) => !api.id)) throw new Error('Save new APIs before applying API order')
  setStatus('Applying API order...')
  setFeedback('busy', 'Applying API order...')
  await serial.rpc(Commands.REORDER_APIS, {
    network_id: network.id,
    ids: network.apis.map((api) => api.id),
  })
  await waitForRestart('API order saved; device is restarting', 'API order saved. Reconnect Serial to continue.')
}

function withFeedback(action) {
  return async () => {
    clearFeedback()
    try {
      await action()
    } catch (err) {
      setStatus(err.message)
      setFeedback('error', err.message)
    }
  }
}

serial.onDisconnect = (reason) => {
  applyDisconnectedUi(
    `${reason}. The ESP32-S3 USB-Serial-JTAG can soft-reset when the port is (re)opened — wait until the e-ink screen shows the provisioning info, then Reconnect Serial.`,
  )
}

connectButton.addEventListener('click', withFeedback(async () => {
  if (!('serial' in navigator)) {
    throw new Error('Web Serial is not available in this browser, use Chrome or Edge over HTTPS/localhost.')
  }
  setFeedback('busy', 'Opening serial port...')
  await serial.connect()
  connectButton.textContent = 'Connect Serial'
  enableControls(true)
  setStatus('Serial connected')
  setFeedback('success', 'Connected. Use Load Config to read current device state.')
}))

disconnectButton.addEventListener('click', withFeedback(async () => {
  setFeedback('busy', 'Closing serial port...')
  await serial.disconnect('Manual disconnect')
  applyDisconnectedUi(null)
  setFeedback('success', 'Disconnected.')
}))

loadButton.addEventListener('click', withFeedback(loadConfig))

addNetworkButton.addEventListener('click', () => {
  if (state.config.networks.length >= MAX_WIFI_NETWORKS) {
    setFeedback('error', `A config can have at most ${MAX_WIFI_NETWORKS} networks.`)
    return
  }
  const network = normalizeNetwork({ id: 0, enabled: true, ssid: '', password: '', apis: [] })
  state.config.networks.push(network)
  state.selectedNetworkKey = network._clientKey
  render()
  setFeedback('busy', 'New network added, fill in SSID + APIs, then Save Highlighted Network.')
})

saveButton.addEventListener('click', withFeedback(saveSelectedNetwork))

reorderNetworksButton.addEventListener('click', withFeedback(async () => {
  setFeedback('busy', 'Applying network order...')
  const ids = state.config.networks.map((network) => network.id).filter(Boolean)
  if (ids.length !== state.config.networks.length) {
    throw new Error('Save new networks before applying network order')
  }
  await serial.rpc(Commands.REORDER_NETWORKS, { ids })
  await waitForRestart('Order saved; device is restarting', 'Network order saved. Reconnect Serial to continue.')
}))

reprovisionButton.addEventListener('click', withFeedback(async () => {
  setFeedback('busy', 'Requesting reprovision...')
  await serial.rpc(Commands.REPROVISION_WIFI)
  await waitForRestart(
    'Reprovision requested; device is restarting',
    'Reprovision requested. Join the device SoftAP and open http://192.168.4.1 after it restarts.',
  )
}))

enableControls(false)
render()
