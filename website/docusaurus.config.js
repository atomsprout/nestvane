import {themes as prismThemes} from 'prism-react-renderer';

// The site is served at https://atomsprout.com/nestvane/ by GitHub Pages.
// Its documents are not kept here: `npm run sync` copies them from ../docs (see scripts/sync-docs.mjs).

/** @type {import('@docusaurus/types').Config} */
const config = {
  title: 'Nestvane',
  tagline: 'Android 17, running inside an app on an iPhone',
  favicon: 'img/favicon.png',

  future: {
    v4: true,
  },

  url: 'https://atomsprout.com',
  baseUrl: '/nestvane/',
  organizationName: 'atomsprout',
  projectName: 'nestvane',
  trailingSlash: true,

  onBrokenLinks: 'throw',
  markdown: {
    // The documents are plain Markdown written for GitHub, not MDX.
    format: 'detect',
    mermaid: true,
    hooks: {
      onBrokenMarkdownLinks: 'throw',
    },
  },

  i18n: {
    defaultLocale: 'en',
    // The site is in English. The repository's README and documents also exist in Chinese.
    locales: ['en'],
  },

  themes: ['@docusaurus/theme-mermaid'],

  presets: [
    [
      'classic',
      /** @type {import('@docusaurus/preset-classic').Options} */
      ({
        docs: {
          sidebarPath: './sidebars.js',
        },
        blog: {
          blogTitle: 'Nestvane blog',
          blogDescription: 'Notes on how Nestvane was built and what was measured.',
          blogSidebarTitle: 'Posts',
          showReadingTime: true,
          onUntruncatedBlogPosts: 'throw',
          onInlineAuthors: 'ignore',
        },
        theme: {
          customCss: './src/css/custom.css',
        },
      }),
    ],
  ],

  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    ({
      image: 'img/nestvane-icon-512.png',
      colorMode: {
        // The site opens light and sunny whatever the system is set to; the switch stays.
        defaultMode: 'light',
        respectPrefersColorScheme: false,
      },
      navbar: {
        title: 'Nestvane',
        logo: {
          alt: 'The Nestvane icon',
          src: 'img/nestvane-icon.png',
        },
        items: [
          {type: 'docSidebar', sidebarId: 'docs', position: 'left', label: 'Documentation'},
          {to: '/docs/installing/', label: 'Install', position: 'left'},
          {to: '/docs/status/', label: 'Status', position: 'left'},
          {to: '/blog/', label: 'Blog', position: 'left'},
          {href: 'https://github.com/atomsprout/nestvane/releases', label: 'Download', position: 'right'},
          {href: 'https://github.com/atomsprout/nestvane', label: 'GitHub', position: 'right'},
        ],
      },
      footer: {
        style: 'light',
        links: [
          {
            title: 'Nestvane',
            items: [
              {label: 'Documentation', to: '/docs/'},
              {label: 'Install the release', to: '/docs/installing/'},
              {label: 'Blog', to: '/blog/'},
              {label: 'Privacy policy', href: 'pathname:///nestvane/#privacy'},
            ],
          },
          {
            title: 'Community',
            items: [
              {label: 'Discord', href: 'https://discord.gg/eGzEaP6TzR'},
              {label: 'Telegram', href: 'https://t.me/atomsprout'},
              {label: 'Email', href: 'mailto:atomsprout@gmail.com?subject=Nestvane%20support'},
            ],
          },
          {
            title: 'Source',
            items: [
              {label: 'GitHub', href: 'https://github.com/atomsprout/nestvane'},
              {label: 'Releases', href: 'https://github.com/atomsprout/nestvane/releases'},
              {label: 'Apache License 2.0', href: 'https://github.com/atomsprout/nestvane/blob/main/LICENSE'},
            ],
          },
        ],
        copyright:
          'Nestvane is made by AtomSprout. Android is a trademark of Google LLC. Nestvane is not affiliated with or endorsed by Google.',
      },
      prism: {
        theme: prismThemes.github,
        darkTheme: prismThemes.dracula,
        additionalLanguages: ['bash', 'cpp', 'armasm'],
      },
    }),
};

export default config;
