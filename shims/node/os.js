/*
 * `node:os`. The suite wants tmpdir(), to build a site somewhere real and
 * throw it away.
 */
export const tmpdir = () => globalThis.__fs_tmpdir() ?? '/tmp';
export const EOL = '\n';
export const platform = () => 'native';
export default { tmpdir, EOL, platform };
