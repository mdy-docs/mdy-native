/* `node:fs/promises` — the promise half of ./fs.js, which is where the
 * implementation lives. Separate module because that is how it is imported. */
export {
  readFile, writeFile, mkdir, mkdtemp, rm, readdir, cp, copyFile, stat, access,
} from './fs.js';
import * as fs from './fs.js';
export default {
  readFile: fs.readFile, writeFile: fs.writeFile, mkdir: fs.mkdir,
  mkdtemp: fs.mkdtemp, rm: fs.rm, readdir: fs.readdir, cp: fs.cp,
  copyFile: fs.copyFile, stat: fs.stat, access: fs.access,
};
