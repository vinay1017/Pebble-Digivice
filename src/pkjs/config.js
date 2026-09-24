module.exports = [
  {
    "type": "heading",
    "defaultValue": "Digivice Settings"
  },
  {
    "type": "toggle",
    "messageKey": "ResetDigimon",
    "label": "Reset My Digimon",
    "description": "Warning: this permanently resets your Digimon back to a fresh Botamon egg. This cannot be undone.",
    "defaultValue": false
  },
  {
    "type": "toggle",
    "messageKey": "BatterySaver",
    "label": "Battery Saver Mode",
    "description": "Turns off the Digimon's idle bouncing animation to save battery. Everything else still works normally.",
    "defaultValue": false
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];