# The Nestvane website

A [Docusaurus](https://docusaurus.io/) site in English, served at <https://atomsprout.com/nestvane/>: a home
page, the documents, and a blog. Chinese readers have the repository's `README.zh-CN.md` and `docs/zh-CN/`.

The documents are not kept here. They live in [`../docs`](../docs), where GitHub shows them as they are, and
`scripts/sync-docs.mjs` copies the English ones into `docs/` before every build. The copy is ignored by git.
Edit the documents in `../docs`.

```sh
npm install
npm run start      # with live reload
npm run build      # into build/; fails on a broken link
npm run serve      # serve build/ at http://localhost:3000/nestvane/
```

- `src/pages/index.js` and `src/home-content.js` are the home page and its words.
  The iOS app links to `#privacy` on it: keep that id, and keep the policy text word for word.
- `sidebars.js` orders the documents. A new document in `../docs` must be added there.
- `blog/` holds the posts, one folder each with its pictures. A post needs a `<!-- truncate -->` line after its
  opening paragraphs. The drawings are made from the pages in `blog-figures/`.
- `src/css/custom.css` holds the colours. The site opens in the light theme.
- `.github/workflows/pages.yml` builds and publishes the site.
