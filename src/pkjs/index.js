var Clay = require('@rebble/clay');
var clayConfig = require('./config');
// autoHandleEvents: false - we need to inspect the settings ourselves
// (checking for the reset toggle) rather than have Clay auto-send
// everything, same pattern used for the Tamagotchi emulator's own reset
// toggle.
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

Pebble.addEventListener('showConfiguration', function(e) {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) return;

  var dict = clay.getSettings(e.response);

  if (dict['ResetDigimon'] === true) {
    Pebble.sendAppMessage({ 'ResetDigimon': 1 });
    // Clear the stored value so it doesn't silently stay "checked" and
    // fire again the next time settings are opened and saved.
    clay.setSettings('ResetDigimon', false);
  }

  Pebble.sendAppMessage({ 'BatterySaver': dict['BatterySaver'] === true ? 1 : 0 });
});