# The Nestvane website

A [Docusaurus](https://docusaurus.io/) site, in English and 简体中文, served at <https://atomsprout.com/nestvane/>.

The documents are not kept here. They live in [`../docs`](../docs), where GitHub shows them as they are, and
`scripts/sync-docs.mjs` copies them in before every build: English to `docs/`, Chinese to the `zh-Hans`
translation folder. Both copies are ignored by git. Edit the documents in `../docs`.

```sh
npm install
npm run start      # English, with live reload
npm run build      # both languages, into build/; fails on a broken link
npm run serve      # serve build/ at http://localhost:3000/nestvane/
```

- `src/pages/index.js` and `src/home-content.js` are the home page and its words in both languages.
  The iOS app links to `#privacy` on it: keep that id, and keep the English policy text word for word.
- `sidebars.js` orders the documents. A new document in `../docs` must be added there.
- `i18n/zh-Hans/` holds the Chinese words of the navigation bar, the footer and the sidebar groups.
- `.github/workflows/pages.yml` builds and publishes the site once the repository is public.
