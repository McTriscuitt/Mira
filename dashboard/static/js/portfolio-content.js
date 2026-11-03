// What the terminal says. Edit this file to change the site's content;
// terminal.js decides how it behaves and shouldn't need touching.
//
// Adding a project: add an entry to `projects`. Its name becomes a command, and
// it appears in `projects`, `help` and tab completion automatically. A project with
// its own directory file (see mira.js) opens in place; any other goes to its `url`.
//
// Anything left empty ('' or null) is left out of the output.

const SITE = {
  name: 'Tristan DeOrnellis',
  tagline: 'software and embedded projects',

  // DRAFT: replace with your own words.
  about: [
    'I build software that reaches past the screen.',
    'Mira, the project here, is firmware and a web dashboard that run the lights in my room.',
  ],

  projects: [
    {
      name: 'mira',
      url: '/mira',
      summary: 'Smart lighting on an ESP32-S3: C++ firmware, Hue bulbs, Flask dashboard',
      helpLine: 'the lights in my room, live',
      // DRAFT: replace with your own words.
      intro: [
        'Mira runs the lights in my room. C++ firmware on an ESP32-S3 fades four Hue bulbs through the day on a timeline, and notices when someone changes them by hand.',
        'This reads the real system, live.',
      ],
    },
  ],

  contact: {
    email: '',       // e.g. 'you@example.com'
    linkedin: '',    // full URL
    github: 'https://github.com/McTriscuitt',
  },

  resume: '',        // a URL, or a path such as '/static/resume.pdf'

  // DRAFT: check this still matches what the site records.
  privacy: [
    'Each visit records the time, your IP address, your browser’s description of itself, the page that linked here, and the commands you type here. Anything that isn’t a known command is stored only as [unknown].',
    'There is no location lookup or fingerprinting, browsing sets no cookies, and records are deleted after 90 days.',
  ],
};
