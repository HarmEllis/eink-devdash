import { Commands, ImprovSerial } from './improv-protocol.js'

const serial = new ImprovSerial()
const state = {
  config: { proto_version: 2, refresh_min: 5, networks: [] },
  selectedNetworkId: null,
}

const statusEl = document.querySelector('#status')
const connectButton = document.querySelector('#connect-button')
const loadButton = document.querySelector('#load-button')
const reprovisionButton = document.querySelector('#reprovision-button')
const addNetworkButton = document.querySelector('#add-network-button')
const saveButton = document.querySelector('#save-button')
const reorderNetworksButton = document.querySelector('#reorder-networks-button')
const networkList = document.querySelector('#network-list')

function setStatus(message) {
  statusEl.textContent = message
}

function enableControls(enabled) {
  loadButton.disabled = !enabled
  reprovisionButton.disabled = !enabled
  addNetworkButton.disabled = !enabled
  saveButton.disabled = !enabled
  reorderNetworksButton.disabled = !enabled
}

function selectedNetwork() {
  return state.config.networks.find((network) => network.id === state.selectedNetworkId)
}

function moveItem(items, index, delta) {
  const next = index + delta
  if (next < 0 || next >= items.length) return
  const [item] = items.splice(index, 1)
  items.splice(next, 0, item)
  render()
}

function render() {
  networkList.replaceChildren()
  for (const [networkIndex, network] of state.config.networks.entries()) {
    const card = document.createElement('article')
    card.className = `network ${network.id === state.selectedNetworkId ? 'selected' : ''}`
    card.addEventListener('click', () => {
      state.selectedNetworkId = network.id
      render()
    })

    const header = document.createElement('div')
    header.className = 'network-header'
    header.innerHTML = `<strong>${network.ssid || 'New network'}</strong>`
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
      inputField('SSID', network.ssid, (value) => { network.ssid = value }),
      inputField('Password', network.password ?? '', (value) => { network.password = value }, 'password'),
    )
    card.append(grid)

    const apiHeader = document.createElement('div')
    apiHeader.className = 'api-header'
    apiHeader.innerHTML = '<strong>APIs</strong>'
    const apiHeaderControls = document.createElement('div')
    apiHeaderControls.className = 'row'
    apiHeaderControls.append(button('Apply API Order', () => reorderApis(network)))
    apiHeaderControls.append(button('Add API', () => {
      network.apis.push({ id: Date.now(), enabled: true, api_url: 'http://', device_token: '' })
      render()
    }))
    apiHeader.append(apiHeaderControls)
    card.append(apiHeader)

    for (const [apiIndex, api] of network.apis.entries()) {
      const row = document.createElement('div')
      row.className = 'api api-grid'
      row.append(
        inputField('API URL', api.api_url, (value) => { api.api_url = value }, 'url'),
        inputField('Token', api.device_token ?? '', (value) => { api.device_token = value }, 'password'),
        apiControls(network, apiIndex),
      )
      card.append(row)
    }
    networkList.append(card)
  }
}

function button(label, onClick) {
  const el = document.createElement('button')
  el.type = 'button'
  el.textContent = label
  el.addEventListener('click', (event) => {
    event.stopPropagation()
    onClick()
  })
  return el
}

function inputField(label, value, onInput, type = 'text') {
  const wrapper = document.createElement('label')
  wrapper.textContent = label
  const input = document.createElement('input')
  input.type = type
  input.value = value ?? ''
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

async function loadConfig() {
  setStatus('Loading config...')
  const config = await serial.rpc(Commands.LIST_CONFIG)
  state.config = config
  state.selectedNetworkId = config.networks[0]?.id ?? null
  render()
  setStatus('Config loaded')
}

async function saveSelectedNetwork() {
  const network = selectedNetwork()
  if (!network) return
  if (!network.ssid) throw new Error('SSID is required')
  if (network.apis.length > 5) throw new Error('A network can have at most 5 APIs')
  for (const api of network.apis) {
    if (!api.api_url.startsWith('http://')) throw new Error('API URLs must start with http://')
  }
  setStatus('Saving network...')
  await serial.rpc(Commands.SET_NETWORK, network)
  setStatus('Saved; device is restarting')
}

async function deleteNetwork(network) {
  if (!network.id) {
    state.config.networks = state.config.networks.filter((item) => item !== network)
    render()
    return
  }
  setStatus('Deleting network...')
  await serial.rpc(Commands.DELETE_NETWORK, { id: network.id })
  setStatus('Deleted; device is restarting')
}

async function reorderApis(network) {
  setStatus('Applying API order...')
  await serial.rpc(Commands.REORDER_APIS, {
    network_id: network.id,
    ids: network.apis.map((api) => api.id),
  })
  setStatus('API order saved; device is restarting')
}

connectButton.addEventListener('click', async () => {
  if (!('serial' in navigator)) {
    setStatus('Web Serial is not available in this browser')
    return
  }
  try {
    await serial.connect()
    enableControls(true)
    setStatus('Serial connected')
  } catch (err) {
    setStatus(err.message)
  }
})

loadButton.addEventListener('click', () => loadConfig().catch((err) => setStatus(err.message)))

addNetworkButton.addEventListener('click', () => {
  const network = { id: Date.now(), enabled: true, ssid: '', password: '', apis: [] }
  state.config.networks.push(network)
  state.selectedNetworkId = network.id
  render()
})

saveButton.addEventListener('click', () => saveSelectedNetwork().catch((err) => setStatus(err.message)))

reorderNetworksButton.addEventListener('click', async () => {
  setStatus('Applying network order...')
  await serial.rpc(Commands.REORDER_NETWORKS, { ids: state.config.networks.map((network) => network.id) })
  setStatus('Order saved; device is restarting')
})

reprovisionButton.addEventListener('click', async () => {
  setStatus('Starting reprovision...')
  await serial.rpc(Commands.REPROVISION_WIFI)
  setStatus('Reprovision requested; device is restarting')
})

enableControls(false)
render()
