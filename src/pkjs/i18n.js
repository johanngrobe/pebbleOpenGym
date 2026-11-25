// Phone-side texts in German. English is the key; anything not listed stays English.
// The watch tells us its language with each sync request; the settings page falls back to the
// phone's language until then.

var DE = {
  'Set up in the Pebble app': 'In der Pebble-App einrichten',
  'Uploading to openGym...': 'Lade zu openGym hoch...',
  'Loading routines...': 'Lade Routinen...',
  'Uploaded {n} workout': '{n} Training hochgeladen',
  'Uploaded {n} workouts': '{n} Trainings hochgeladen',
  'Up to date': 'Aktuell',
  'Server unreachable': 'Server nicht erreichbar',
  'Token refused: check settings': 'Token abgelehnt: Einstellungen prüfen',
  'Token needs Workouts: Change': 'Token braucht Trainings: Ändern',
  'Server refused the request': 'Server hat abgelehnt',
  'No openGym API at that address': 'Keine openGym-API unter der Adresse',
  'Open openGym once, then sync': 'openGym einmal öffnen, dann sync.',
  'Too many requests, try later': 'Zu viele Anfragen, später erneut',
  'Server error {n}': 'Serverfehler {n}',
  'Bad server address': 'Ungültige Serveradresse',
  'Server timed out': 'Server antwortet nicht',

  // Settings page
  'openGym for Pebble': 'openGym für Pebble',
  'Your own openGym server. The watch keeps your routines and logs workouts without the phone; they upload the next time the phone is near.':
    'Dein eigener openGym-Server. Die Uhr speichert deine Routinen und zeichnet Trainings ohne Handy auf; sie werden hochgeladen, sobald das Handy wieder in der Nähe ist.',
  'Server address': 'Serveradresse',
  'API token': 'API-Token',
  'Needs openGym 1.4.5 or newer. Sign in on the website, then: Settings &rarr; Integrations &rarr; API tokens.':
    'Braucht openGym 1.4.5 oder neuer. Auf der Website anmelden, dann: Einstellungen &rarr; Integrationen &rarr; API-Tokens.',
  'Workouts: {a}, Plans &amp; routines: {b}.': 'Trainings: {a}, Pläne &amp; Routinen: {b}.',
  'Exercise names': 'Übungsnamen',
  'Like the openGym profile': 'Wie im openGym-Profil',
  'Weight step on the watch': 'Gewichtsschritt auf der Uhr',
  'Default (2.5 kg / 5 lb)': 'Standard (2,5 kg / 5 lb)',
  'Vibration when a timer ends': 'Vibration, wenn ein Timer abläuft',
  'Normal (two pulses)': 'Normal (zwei Impulse)',
  'Strong (three long pulses)': 'Stark (drei lange Impulse)',
  'Short (one pulse)': 'Kurz (ein Impuls)',
  'Off': 'Aus',
  'For the rest timer and timed holds. Off also drops the nudge 10 seconds before a rest ends.':
    'Für den Pausentimer und Halteübungen. Aus schaltet auch den Hinweis 10 Sekunden vor Ende der Pause ab.',
  'Save': 'Speichern'
};

// 'de' for any German locale ("de", "de-AT", "de_CH"), otherwise 'en'.
function norm(lang) {
  return /^de/i.test(String(lang || '')) ? 'de' : 'en';
}

function t(lang, text, vars) {
  var out = (norm(lang) === 'de' && DE[text]) || text;
  if (vars) {
    out = out.replace(/\{(\w+)\}/g, function (m, k) { return k in vars ? vars[k] : m; });
  }
  return out;
}

module.exports = { t: t, norm: norm };
