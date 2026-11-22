// The documents live in ../docs, where GitHub shows them as they are. This copies the English ones to
// docs/, where Docusaurus reads them. On the way it turns links that leave the documents (to source
// files, the lab record, the license, the Chinese documents) into links to the repository on GitHub,
// since the site has no page for them.
import {cpSync, mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const source = join(here, '..', '..', 'docs');
const english = join(here, '..', 'docs');
const repository = 'https://github.com/atomsprout/nestvane/blob/main/';

rmSync(english, {recursive: true, force: true});
mkdirSync(english, {recursive: true});
cpSync(join(source, 'images'), join(english, 'images'), {recursive: true});
for (const name of readdirSync(source).filter(file => file.endsWith('.md'))) {
  let text = readFileSync(join(source, name), 'utf8');
  text = text.replaceAll('](zh-CN/', `](${repository}docs/zh-CN/`);
  text = text.replaceAll('](../', `](${repository}`);
  if (text.includes('](../')) throw new Error(`${name}: a link still leaves the documents`);
  writeFileSync(join(english, name), text);
}
console.log('documents copied from ../docs');
