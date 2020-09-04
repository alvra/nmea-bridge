
var ws
var log
var input
var button
var indicator
var attempt = 0

var reconnect_delay = 1000


function start() {
    console.log("connecting")
    setConnected(false)
    ws = new WebSocket("ws://" + window.location.host + ':' + log.dataset['port'])
    ws.onmessage = onMessage
    ws.onopen = onOpen
    ws.onerror = onError
    ws.onclose = onClose
}

function getStartDelay() {
    // in milliseconds
    if (attempt == 0) {
        return 0
    } else {
        return reconnect_delay
    }
}

function onOpen() {
    console.log("open")
    attempt = 0
    setConnected(true)
}

function onError() {
    console.log("error", attempt)
    attempt += 1
    setTimeout(start, getStartDelay())
}

function onClose() {
    console.log("close")
    setTimeout(start, getStartDelay())
}

function onMessage(event) {
    console.log("data", event.data)
    var z = document.getElementById('empty')
    if (z) {z.remove()}
    var data = JSON.parse(event.data)
    var e = document.createElement('div')
    e.textContent = data.sentence
    e.className = 'from-' + data.source
    var scrolledAtBottom = log.clientHeight + log.scrollTop == log.scrollHeight
    log.appendChild(e)
    if (scrolledAtBottom) {
        // if the log was scrolled to the bottom before,
        // set it to the bottom afterwards also
        log.scrollTop = log.scrollHeight
    }
}

function onSend(){
    if (ws && ws.readyState == WebSocket.OPEN) {
        console.log("send", input.value)
        ws.send(input.value + '\r\n')
        input.value = ''
    }
    return false
}


function setConnected(state) {
    if (state) {
        indicator.classList.add('on')
    } else {
        indicator.classList.remove('on')
    }
    indicator.textContent = state ? 'connected' : 'disconnected'
    button.disabled = ! state
}

window.addEventListener('load', function(){
    log = document.getElementById('log')
    input = document.getElementById('in')
    button = document.getElementById('send')
    indicator = document.getElementById('indicator')
    document.forms[0].onsubmit = onSend
    start()
});

