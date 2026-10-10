var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

var PENDING_KEY = 'digivicePendingReset';

function flag(settings, name) {
  var v = settings[name];
  if (v && typeof v === 'object') v = v.value;
  return v === true || v === 1 || v === '1' || v === 'true';
}

function getPending() {
  try { return localStorage.getItem(PENDING_KEY) === '1'; } catch (e) { return false; }
}
function setPending(on) {
  try {
    if (on) localStorage.setItem(PENDING_KEY, '1');
    else localStorage.removeItem(PENDING_KEY);
  } catch (e) {}
}

function sendToWatch(payload, includesReset) {
  console.log('Sending to watch: ' + JSON.stringify(payload));
  Pebble.sendAppMessage(payload,
    function() {
      console.log('Watch ACKed the message');
      if (includesReset) setPending(false);
    },
    function(err) {
      console.log('Send FAILED: ' + JSON.stringify(err) +
        (includesReset ? ' - reset stays queued and will retry next time the Digivice app opens' : ''));
    });
}

// Whenever the Digivice app (re)starts on the watch, deliver any reset
// that couldn't be delivered earlier.
Pebble.addEventListener('ready', function() {
  if (getPending()) {
    console.log('Pending reset found - sending now');
    sendToWatch({ 'ResetDigimon': 1 }, true);
  }
});

Pebble.addEventListener('showConfiguration', function() {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) return;

  // convert=false: settings keyed by the NAMES from config.js, each as
  // {value: ...}, with no id/number conversion to get wrong.
  var raw = clay.getSettings(e.response, false);
  console.log('Raw settings from config page: ' + JSON.stringify(raw));

  var wantsReset = flag(raw, 'ResetDigimon');
  var batterySaver = flag(raw, 'BatterySaver');
  console.log('Parsed -> reset: ' + wantsReset + ', batterySaver: ' + batterySaver);

  var payload = { 'BatterySaver': batterySaver ? 1 : 0 };
  if (wantsReset) {
    payload['ResetDigimon'] = 1;
    setPending(true);                       // stays queued until the watch ACKs
    clay.setSettings('ResetDigimon', false); // so the toggle isn't left on
  }
  sendToWatch(payload, wantsReset);
});