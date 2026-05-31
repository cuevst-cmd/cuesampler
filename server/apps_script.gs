/**
 * CUE SAMPLER telemetry receiver — Google Apps Script Web App.
 *
 * Receives newline-delimited JSON (JSONL) POSTed by the plugin and appends one
 * row per event to the active spreadsheet. No server to run; Google hosts it.
 *
 * SETUP
 *  1. Create a new Google Sheet (sheets.new). This is where data lands.
 *  2. Extensions → Apps Script. Delete the placeholder, paste this whole file.
 *  3. Set SECRET below to any random string. Put the SAME string in your
 *     plugin's APIKeys.h as TelemetrySharedSecret.
 *  4. Deploy → New deployment → type "Web app".
 *       - Execute as:  Me
 *       - Who has access:  Anyone
 *     Click Deploy, authorize, and copy the "/exec" Web app URL.
 *  5. Paste that URL into APIKeys.h as TelemetryEndpointUrl, rebuild the plugin.
 *
 * Re-deploy (Manage deployments → edit → new version) after editing this script.
 */

const SECRET = 'CHANGE_ME_to_a_random_string';

// Columns written, in order. Add/rename freely — header is written once.
const HEADER = ['ts', 'type', 'install', 'fp', 'durSec', 'sr', 'ch',
                'algoBpm', 'userBpm', 'trim', 'conf', 'drifting',
                'localKey', 'localConf', 'metadataKey', 'onlineKey', 'localCorrect'];

function doPost(e) {
  try {
    if (SECRET && (!e || !e.parameter || e.parameter.secret !== SECRET))
      return json_({ ok: false, err: 'bad secret' });

    const ss = SpreadsheetApp.getActiveSpreadsheet();
    let sheet = ss.getSheetByName('events');
    if (!sheet) {
      sheet = ss.insertSheet('events');
      sheet.appendRow(HEADER);
    }

    const body = (e.postData && e.postData.contents) ? e.postData.contents : '';
    const lines = body.split('\n');
    let added = 0;

    lines.forEach(function (line) {
      line = line.trim();
      if (!line) return;
      let o;
      try { o = JSON.parse(line); } catch (_) { return; }
      sheet.appendRow(HEADER.map(function (k) {
        return (o[k] === undefined || o[k] === null) ? '' : o[k];
      }));
      added++;
    });

    return json_({ ok: true, added: added });
  } catch (err) {
    return json_({ ok: false, err: String(err) });
  }
}

// Lets you sanity-check the deployment in a browser (GET shows it's alive).
function doGet() {
  return json_({ ok: true, msg: 'CUE SAMPLER telemetry endpoint is live' });
}

function json_(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
