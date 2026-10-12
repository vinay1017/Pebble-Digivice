var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

// ---- Save format (must match export/import in the watch's digivice.c) ----
// Watch <-> phone text: comma-separated integers in exactly this order,
// preceded by a version number (SAVE_VERSION).
var SAVE_VERSION = 1;
var FIELD_ORDER = [
  'species', 'hunger', 'energy', 'strength', 'age_hours', 'hours_in_stage',
  'steps_in_stage', 'training_count', 'alive', 'has_mess', 'hours_since_clean',
  'care_mistakes', 'manually_asleep', 'vital_points', 'win_count', 'loss_count',
  'last_steps_shortfall', 'last_effort_shortfall'
];
var BOOL_FIELDS = { alive: true, has_mess: true, manually_asleep: true };
// Index = species ID on the watch (the Species enum). Append only.
var SPECIES = [
  'Botamon', 'Koromon', 'Agumon', 'Greymon', 'MetalGreymon', 'Dokimon',
  'Bibimon', 'Betamon', 'Pulsemon', 'Tyranomon', 'Airdramon', 'Seadramon',
  'Meramon', 'Devimon', 'Numemon', 'Kazuchimon', 'BlitzGreymon', 'Mamemon',
  'Monzaemon', 'Boutmon', 'BanchoMamemon', 'ShinMonzaemon', 'Bulkmon'
];

var PENDING_RESET = 'digivicePendingReset';
var PENDING_IMPORT = 'digivicePendingImport';
var SNAPSHOT = 'digiviceSnapshot';
var SNAPSHOT_TIME = 'digiviceSnapshotTime';

function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
function lsSet(k, v) { try { localStorage.setItem(k, v); } catch (e) {} }
function lsDel(k) { try { localStorage.removeItem(k); } catch (e) {} }

function flag(settings, name) {
  var v = settings[name];
  if (v && typeof v === 'object') v = v.value;
  return v === true || v === 1 || v === '1' || v === 'true';
}
function text(settings, name) {
  var v = settings[name];
  if (v && typeof v === 'object') v = v.value;
  return (typeof v === 'string') ? v.trim() : '';
}

// ---- Watch text -> object ----
function parseSnapshot(csv) {
  if (typeof csv !== 'string') return null;
  var parts = csv.split(',');
  if (parts.length !== FIELD_ORDER.length + 1) return null;
  var nums = parts.map(function(p) { return parseInt(p, 10); });
  for (var i = 0; i < nums.length; i++) if (isNaN(nums[i])) return null;
  if (nums[0] !== SAVE_VERSION) return null;
  var obj = {};
  for (var f = 0; f < FIELD_ORDER.length; f++) {
    var name = FIELD_ORDER[f];
    var n = nums[f + 1];
    if (name === 'species') obj.species = SPECIES[n] || ('Unknown(' + n + ')');
    else if (BOOL_FIELDS[name]) obj[name] = n !== 0;
    else obj[name] = n;
  }
  return obj;
}

// ---- Object (pasted JSON) -> watch text ----
function speciesToId(v) {
  if (typeof v === 'number' && isFinite(v) && v >= 0 && v < SPECIES.length && Math.floor(v) === v) return v;
  if (typeof v === 'string') {
    var want = v.replace(/\s+/g, '').toLowerCase();
    for (var i = 0; i < SPECIES.length; i++) if (SPECIES[i].toLowerCase() === want) return i;
  }
  return -1;
}
// Phone keyboards love to swap straight quotes for curly ones and leave
// trailing commas; repair both before parsing.
function tidyJson(t) {
  return t.replace(/[\u201C\u201D\u201E\u201F]/g, '"')
          .replace(/[\u2018\u2019]/g, "'")
          .replace(/,\s*([}\]])/g, '$1');
}

// The pasted JSON may be a COMPLETE export or just the fields you want to
// change (e.g. {"species": "Agumon"}) - anything left out is taken from
// the latest snapshot of your Digimon.
function jsonToCsv(jsonText) {
  var obj;
  try { obj = JSON.parse(tidyJson(jsonText)); } catch (e) { return { error: 'not valid JSON (' + e.message + ')' }; }
  if (!obj || typeof obj !== 'object' || Array.isArray(obj)) return { error: 'not a JSON object' };
  if (obj.version !== undefined && obj.version !== SAVE_VERSION) return { error: 'unsupported version ' + obj.version };
  var base = parseSnapshot(lsGet(SNAPSHOT)) || {};
  var out = [SAVE_VERSION];
  for (var f = 0; f < FIELD_ORDER.length; f++) {
    var name = FIELD_ORDER[f];
    var v = obj[name];
    if (v === undefined) v = base[name];
    if (v === undefined) return { error: 'missing field "' + name + '" (and no snapshot to fill it from)' };
    if (name === 'species') {
      var id = speciesToId(v);
      if (id < 0) return { error: 'unknown species "' + v + '"' };
      out.push(id);
    } else if (BOOL_FIELDS[name]) {
      if (v === true || v === 1 || v === 'true') out.push(1);
      else if (v === false || v === 0 || v === 'false') out.push(0);
      else return { error: 'field "' + name + '" must be true or false' };
    } else {
      if (typeof v !== 'number' || !isFinite(v) || Math.floor(v) !== v) return { error: 'field "' + name + '" must be a whole number' };
      out.push(v);
    }
  }
  return { csv: out.join(',') };
}

function escapeHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// ---- Settings page (built fresh each time it opens) ----
function exportHtml() {
  var snap = parseSnapshot(lsGet(SNAPSHOT));
  if (!snap) {
    return '<p>No save snapshot yet. Open the Digivice on your watch for a few seconds, then reopen these settings.</p>';
  }
  var t = parseInt(lsGet(SNAPSHOT_TIME) || '0', 10);
  var out = { app: 'digivice', version: SAVE_VERSION };
  if (t) out.exported_at = new Date(t).toISOString();
  Object.keys(snap).forEach(function(k) { out[k] = snap[k]; });
  return '<p>Last saved from your watch' + (t ? ' ' + escapeHtml(new Date(t).toLocaleString()) : '') +
    '. Touch and hold the text below to select and copy it.</p>' +
    '<pre style="white-space:pre-wrap;word-break:break-all;font-size:12px;user-select:text;-webkit-user-select:text;">' +
    escapeHtml(JSON.stringify(out, null, 2)) + '</pre>';
}

function buildConfig() {
  var cfg = clayConfig.slice();
  var section = [
    {
      type: 'select',
      messageKey: 'NewDigimon',
      label: 'Start a new Digimon',
      description: 'WARNING: choosing an egg ERASES your current Digimon (all stats and progress) and hatches a fresh one when you press Save. Leave on "Keep my current Digimon" to skip.',
      defaultValue: '',
      options: [
        { label: 'Keep my current Digimon', value: '' },
        { label: 'Hatch a Botamon', value: 'botamon' },
        { label: 'Hatch a Dokimon', value: 'dokimon' }
      ]
    },
    {
      type: 'toggle',
      messageKey: 'ConfirmErase',
      label: "Yes, I'm sure: erase my current Digimon",
      description: 'Must be ON for the egg choice above to take effect. It switches itself back off after every Save.',
      defaultValue: false
    },
    { type: 'heading', defaultValue: 'Backup & Restore' },
    { type: 'text', defaultValue: exportHtml() },
    {
      type: 'input',
      messageKey: 'ImportData',
      label: 'Import a save',
      description: 'Paste a full export, or just the fields to change, like {"species": "Agumon"}. Press Save. This changes your current Digimon. Leave empty to skip.',
      defaultValue: '',
      attributes: { placeholder: '{"species": "Agumon"}' }
    }
  ];
  var at = cfg.length;
  for (var i = 0; i < cfg.length; i++) if (cfg[i].type === 'submit') at = i;
  Array.prototype.splice.apply(cfg, [at, 0].concat(section));
  return cfg;
}

// ---- Sending to the watch ----
function sendToWatch(payload, hasReset, hasImport) {
  console.log('Sending to watch: ' + JSON.stringify(payload));
  Pebble.sendAppMessage(payload,
    function() {
      console.log('Watch ACKed the message');
      if (hasReset) lsDel(PENDING_RESET);
      if (hasImport) lsDel(PENDING_IMPORT);
    },
    function(err) {
      console.log('Send FAILED: ' + JSON.stringify(err) +
        ((hasReset || hasImport) ? ' - queued, will retry next time the Digivice app opens' : ''));
    });
}

function notify(msg) {
  console.log('Digivice: ' + msg);
  try { Pebble.showSimpleNotificationOnPebble('Digivice', msg); } catch (e) {}
}

// When the Digivice (re)starts on the watch, deliver anything still queued.
Pebble.addEventListener('ready', function() {
  var payload = {};
  var resetMode = parseInt(lsGet(PENDING_RESET) || '0', 10) || 0;
  var hasReset = resetMode > 0;
  var pendingImport = lsGet(PENDING_IMPORT);
  if (hasReset) payload.ResetDigimon = resetMode;
  if (pendingImport) payload.SaveData = pendingImport;
  if (hasReset || pendingImport) {
    console.log('Queued changes found - sending now');
    sendToWatch(payload, hasReset, !!pendingImport);
  }
});

// Snapshots arriving from the watch: keep the newest for the Export box.
Pebble.addEventListener('appmessage', function(e) {
  var csv = e && e.payload && e.payload.SaveData;
  if (typeof csv === 'string' && parseSnapshot(csv)) {
    lsSet(SNAPSHOT, csv);
    lsSet(SNAPSHOT_TIME, String(Date.now()));
    console.log('Snapshot stored: ' + csv);
  }
});

Pebble.addEventListener('showConfiguration', function() {
  clay = new Clay(buildConfig(), null, { autoHandleEvents: false });
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) return;

  // convert=false: settings keyed by the NAMES from config.js, each as
  // {value: ...}, with no id/number conversion to get wrong.
  var raw = clay.getSettings(e.response, false);
  console.log('Raw settings from config page: ' + JSON.stringify(raw));

  var batterySaver = flag(raw, 'BatterySaver');
  var importText = text(raw, 'ImportData');
  var newEgg = text(raw, 'NewDigimon').toLowerCase();
  var confirmed = flag(raw, 'ConfirmErase');

  // Safety net: an egg only counts if the confirm toggle is on too.
  if (newEgg && !confirmed) {
    notify('New Digimon NOT started: turn on "Yes, I\'m sure" to confirm the erase.');
    newEgg = '';
  }

  // ResetDigimon values the watch understands:
  //   1 = reset, then ask which egg (the old toggle)
  //   2 = hatch a fresh Botamon   3 = hatch a fresh Dokimon
  var resetMode = 0;
  if (newEgg === 'botamon') resetMode = 2;
  else if (newEgg === 'dokimon') resetMode = 3;
  else if (flag(raw, 'ResetDigimon')) resetMode = 1;
  var wantsReset = resetMode > 0;

  var payload = { 'BatterySaver': batterySaver ? 1 : 0 };
  var hasImport = false;

  if (wantsReset) {
    payload.ResetDigimon = resetMode;
    lsSet(PENDING_RESET, String(resetMode));   // stays queued until the watch ACKs
    clay.setSettings('ResetDigimon', false);   // so the toggle isn't left on
  }
  clay.setSettings('ConfirmErase', false);     // confirmation never carries over to the next Save
  clay.setSettings('NewDigimon', '');          // dropdown returns to "Keep my current Digimon"

  // Starting a new Digimon wins over a pasted import - they'd contradict.
  if (importText && resetMode >= 2) {
    notify('Import ignored: you chose to start a new Digimon.');
    importText = '';
  }

  if (importText) {
    var result = jsonToCsv(importText);
    if (result.error) {
      notify('Import failed: ' + result.error);
    } else {
      payload.SaveData = result.csv;
      lsSet(PENDING_IMPORT, result.csv);
      hasImport = true;
    }
  }
  clay.setSettings('ImportData', ''); // never keep the pasted text, or it would re-import on every Save

  sendToWatch(payload, wantsReset, hasImport);
});