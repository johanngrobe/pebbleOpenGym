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
  'Focus at workout start': 'Fokus beim Trainingsstart',
  'Remind me to turn on Do Not Disturb': 'An „Nicht stören“ erinnern',
  "Pebble apps can't silence notifications themselves: the watch shows a reminder when a workout starts, so you can switch on Do Not Disturb.":
    'Pebble-Apps können Mitteilungen nicht selbst stummschalten: Die Uhr erinnert beim Trainingsstart daran, „Nicht stören“ einzuschalten.',
  'Heart rate during workouts': 'Herzfrequenz im Training',
  'Every second': 'Jede Sekunde',
  'Every 5 seconds': 'Alle 5 Sekunden',
  'Every 10 seconds': 'Alle 10 Sekunden',
  'Every 30 seconds': 'Alle 30 Sekunden',
  'Shown in the top right of the workout. How often the sensor measures during a workout: more often uses more battery. Off leaves the sensor alone.':
    'Oben rechts im Training. Wie oft der Sensor während des Trainings misst: öfter braucht mehr Akku. Aus lässt den Sensor in Ruhe.',
  'Text size on the watch': 'Textgröße auf der Uhr',
  'Like the watch': 'Wie auf der Uhr',
  'Large': 'Groß',
  'Extra large': 'Sehr groß',
  "Like the watch follows the text size in the watch's settings: Extra large there makes the app large too.":
    'Wie auf der Uhr folgt der Textgröße in den Einstellungen der Uhr: Dort sehr groß macht auch die App groß.',
  'Theme on the watch': 'Design auf der Uhr',
  'Dark': 'Dunkel',
  'Light': 'Hell',
  'Accent color': 'Akzentfarbe',
  'openGym green': 'openGym-Grün',
  'Classic green': 'Klassisches Grün',
  'Blue': 'Blau',
  'Orange': 'Orange',
  'Purple': 'Lila',
  'Pink': 'Pink',
  'Red': 'Rot',
  'Teal': 'Türkis',
  'Yellow': 'Gelb',
  "Like the openGym profile takes the theme and color from openGym's settings with each sync, your own color too, as close as the watch can show it. System there is dark on the watch.":
    'Wie im openGym-Profil übernimmt Design und Farbe bei jedem Sync aus den openGym-Einstellungen, auch eine eigene Farbe, so nah, wie die Uhr sie zeigen kann. System ist auf der Uhr dunkel.',
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
