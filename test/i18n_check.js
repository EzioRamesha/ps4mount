#!/usr/bin/env node
/* Checks the panel translations in web/index.html: every language has the same texts with
   the same {values}, and every English text of the panel (page and script) has a translation.
   Run by test/run.sh when node is installed: node test/i18n_check.js */
'use strict';
const fs = require('fs');
const path = require('path');

const html = fs.readFileSync(path.join(__dirname, '..', 'web', 'index.html'), 'utf8');
/* the head holds a small script (the /legacy redirect): the panel's own script and the
   page markup both start after <body> */
const bodyAt = html.indexOf('<body>');
const scriptAt = html.indexOf('<script>', bodyAt);
const script = html.slice(scriptAt + 8, html.lastIndexOf('</script>'));
const page = html.slice(bodyAt, scriptAt);

/* Texts that stay the same in every language. */
const SAME = new Set(['PS4Mount', 'English', 'Español', 'Português (Brasil)', 'Français', '/Backup']);

let failures = 0;
function fail(msg) { failures++; process.stderr.write('FAIL ' + msg + '\n'); }

const i18nSrc = script.slice(script.indexOf('var I18N = '), script.indexOf('\n};\n') + 3);
const I18N = new Function(i18nSrc + '; return I18N;')();
const langs = Object.keys(I18N);
const keys = Object.keys(I18N[langs[0]]);

function holes(s) { return (s.match(/\{\w+\}/g) || []).sort().join(','); }
langs.forEach(function (l) {
  const d = I18N[l];
  keys.forEach(function (k) {
    if (!(k in d)) fail(l + ': missing "' + k + '"');
    else if (holes(d[k]) !== holes(k)) fail(l + ': values differ in "' + k + '"');
    else if (!d[k].trim()) fail(l + ': empty "' + k + '"');
  });
  Object.keys(d).forEach(function (k) { if (keys.indexOf(k) < 0) fail(l + ': extra "' + k + '"'); });
});

const needed = new Set();
/* String literals of the first argument of every t(...) call, joining 'a' + 'b'. */
const code = script.slice(script.indexOf('function langPicked'));
const call = /\bt\(/g;
let m;
while ((m = call.exec(code))) {
  let i = m.index + 2, depth = 0, quote = null, arg = '';
  for (; i < code.length; i++) {
    const c = code[i];
    if (quote) { arg += c; if (c === '\\') { arg += code[++i]; } else if (c === quote) quote = null; continue; }
    if (c === "'" || c === '"') { quote = c; arg += c; continue; }
    if (c === '(' || c === '{' || c === '[') depth++;
    if (c === ')' || c === '}' || c === ']') { if (!depth) break; depth--; }
    if (c === ',' && !depth) break;
    arg += c;
  }
  const joined = arg.replace(/'\s*\+\s*'/g, '');
  const lits = joined.match(/'(?:[^'\\]|\\.)*'/g) || [];
  lits.forEach(function (l) { needed.add(new Function('return ' + l)()); });
}
/* Texts kept in tables and translated where they are shown. */
['labels', 'kinds', 'addonLabels', 'ask', 'roleNames'].forEach(function (name) {
  const r = new RegExp('var ' + name + ' = (\\{[\\s\\S]*?\\})(;|\\[)');
  const found = r.exec(script);
  if (!found) { fail('table ' + name + ' not found'); return; }
  const table = new Function('return ' + found[1])();
  Object.keys(table).forEach(function (k) { needed.add(table[k]); });
});
/* Text written in the page, and its placeholders, labels and titles. */
page.replace(/<(textarea|pre)[^>]*>[\s\S]*?<\/\1>/g, '').replace(/<[^>]*>/g, '\0').split('\0').forEach(function (s) {
  s = s.replace(/\s+/g, ' ').trim();
  if (/[A-Za-z]/.test(s)) needed.add(s);
});
page.replace(/(?:placeholder|aria-label|title)="([^"]*)"/g, function (all, v) { needed.add(v); return all; });

needed.forEach(function (s) {
  if (!SAME.has(s) && keys.indexOf(s) < 0) fail('no translation for "' + s + '"');
});

if (failures) { process.stderr.write(failures + ' translation check(s) failed\n'); process.exit(1); }
process.stdout.write('i18n: ' + langs.length + ' languages, ' + keys.length + ' texts, ' + needed.size + ' used: all translated\n');
