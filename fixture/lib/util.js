import { upper } from "./case.js";
export const shout = (s) => upper(s) + "!";
export const slug = (s) => s.toLowerCase().replace(/[^a-z0-9]+/g, "-");
