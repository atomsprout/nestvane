// The documents live in ../docs, where GitHub shows them as they are. This copies them to where
// Docusaurus reads them: English to docs/, Chinese to the zh-Hans translation folder. On the way it
// turns links that leave the documents (to source files, the lab record, the license) into links to
// the repository on GitHub, since the site has no page for them.
import {cpSync, mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const source = join(here, '..', '..', 'docs');
const english = join(here, '..', 'docs');
const chinese = join(here, '..', 'i18n', 'zh-Hans', 'docusaurus-plugin-content-docs', 'current');
const repository = 'https://github.com/atomsprout/nestvane/blob/main/';

function copy(from, to, up, otherLanguage) {
  rmSync(to, {recursive: true, force: true});
  mkdirSync(to, {recursive: true});
  cpSync(join(source, 'images'), join(to, 'images'), {recursive: true});
  for (const name of readdirSync(from).filter(file => file.endsWith('.md'))) {
    let text = readFileSync(join(from, name), 'utf8');
    // The other language is a switch in the page's header, not a document of this set.
    text = text.replace(otherLanguage.link, otherLanguage.to);
    if (up === '../../') text = text.replaceAll('](../images/', '](./images/');
    text = text.replaceAll(`](${up}`, `](${repository}`);
    if (text.includes('](../')) throw new Error(`${name}: a link still leaves the documents`);
    writeFileSync(join(to, name), text);
  }
}

copy(source, english, '../', {link: '](zh-CN/README.md)', to: '](pathname:///nestvane/zh-Hans/docs/)'});
copy(join(source, 'zh-CN'), chinese, '../../', {link: '](../README.md)', to: '](pathname:///nestvane/docs/)'});
console.log('documents copied from ../docs');
