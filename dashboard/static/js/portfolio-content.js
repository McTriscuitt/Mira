// What the terminal says. Edit this file to change the site's content;
// terminal.js decides how it behaves and shouldn't need touching.
//
// Adding a project: add an entry to `projects`. Its name becomes a command, and
// it appears in `projects`, `help` and tab completion automatically.
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
    },
  ],

  contact: {
    email: '',       // e.g. 'you@example.com'
    linkedin: '',    // full URL
    github: 'https://github.com/McTriscuitt',
  },

  resume: '',        // a URL, or a path such as '/static/resume.pdf'
};
