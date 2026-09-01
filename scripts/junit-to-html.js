#!/usr/bin/env node
/**
 * scripts/junit-to-html.js — Convert JUnit XML test results to a self-contained HTML report
 *
 * Reads JUnit XML from one or more files (or stdin) and writes a single HTML file
 * with Mermaid charts, per-module tables, and pass/fail summary.
 *
 * Usage:
 *   node scripts/junit-to-html.js test-results.xml > report.html
 *   node scripts/junit-to-html.js build/test-results.xml -o report.html
 *   ctest --test-dir build --output-junit results.xml && node scripts/junit-to-html.js results.xml -o report.html
 *
 * Options:
 *   -o, --output <file>   Write HTML to file (default: stdout)
 *   -t, --title  <text>   Report title (default: "Test Results")
 *   -c, --commit <sha>    Commit SHA to display (default: none)
 *   -r, --run    <url>    CI run URL to link (default: none)
 *   --help                Show this help
 */

const fs = require("node:fs");
const path = require("node:path");

// ── CLI argument parsing ────────────────────────────────────────────────────────
const args = process.argv.slice(2);
let outputFile = null;
let title = "Test Results";
let commitSha = null;
let runUrl = null;
const inputFiles = [];

for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (a === "-o" || a === "--output") { outputFile = args[++i]; }
    else if (a === "-t" || a === "--title") { title = args[++i]; }
    else if (a === "-c" || a === "--commit") { commitSha = args[++i]; }
    else if (a === "-r" || a === "--run") { runUrl = args[++i]; }
    else if (a === "--help") { console.log("Usage: node junit-to-html.js [options] [file ...]\n\nOptions:\n  -o, --output <file>   Write HTML to file (default: stdout)\n  -t, --title  <text>   Report title\n  -c, --commit <sha>    Commit SHA\n  -r, --run    <url>    CI run URL\n  --help                Show this help\n\nIf no input files, reads from stdin."); process.exit(0); }
    else if (a.startsWith("-")) { console.error("Unknown option: " + a); process.exit(1); }
    else { inputFiles.push(a); }
}

// ── XML parsing ──────────────────────────────────────────────────────────────────
function parseJUnit(xml) {
    const cases = [];
    const re = /<testcase\s+name="([^"]*)"\s+classname="([^"]*)"\s+time="([^"]*)"\s*(?:status="([^"]*)")?\s*>/g;
    let m;
    while ((m = re.exec(xml)) !== null) {
        const name = m[1];
        const className = m[2];
        const time = parseFloat(m[3]) || 0;
        const status = m[4] || "run";
        const endTag = xml.indexOf("</testcase>", m.index);
        const seg = endTag !== -1 ? xml.substring(m.index + m[0].length, endTag) : "";
        let assertions = 0;
        const am = seg.match(/(\d+)\s+assertions?/);
        if (am) assertions = parseInt(am[1], 10);
        let failureDetail = "";
        let isFailure = false;
        if (seg.includes("<failure")) {
            isFailure = true;
            const fm = seg.match(/<failure[^>]*message="([^"]*)"/);
            failureDetail = fm ? fm[1] : "unknown failure";
        } else if (seg.includes("<error")) {
            isFailure = true;
            const em = seg.match(/<error[^>]*message="([^"]*)"/);
            failureDetail = em ? em[1] : "unknown error";
        } else if (status !== "run") {
            isFailure = true;
            failureDetail = "Status: " + status;
        }
        const module = className.split(":")[0] || "other";
        const shortName = name.startsWith(module + ":") ? name.substring(module.length + 1) : name;
        cases.push({ name, shortName, module, time, assertions, status, isFailure, failureDetail });
    }
    return cases;
}

// ── Aggregation ──────────────────────────────────────────────────────────────────
function buildStats(cases) {
    const byModule = {};
    for (const c of cases) {
        if (!byModule[c.module]) byModule[c.module] = { tests: 0, failed: 0, time: 0, assertions: 0 };
        byModule[c.module].tests++;
        if (c.isFailure) byModule[c.module].failed++;
        byModule[c.module].time += c.time;
        byModule[c.module].assertions += c.assertions;
    }
    return byModule;
}

function totalTime(stats) { return Object.values(stats).reduce((a, b) => a + b.time, 0); }
function totalAssertions(stats) { return Object.values(stats).reduce((a, b) => a + b.assertions, 0); }

// ── HTML generation ──────────────────────────────────────────────────────────────
function esc(s) {
    return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

function modTable(stats) {
    return Object.entries(stats).sort(([a], [b]) => a.localeCompare(b)).map(([mod, s]) =>
        "<tr><td>" + esc(mod) + "</td><td>" + s.tests + "</td><td>" + s.time.toFixed(1) + "s</td><td>" + s.assertions + "</td><td>" + s.failed + "</td></tr>"
    ).join("");
}

function slowRows(cases, n) {
    return [...cases].sort((a, b) => b.time - a.time).slice(0, n).map(c =>
        '<tr class="' + (c.isFailure ? "fail" : "") + '"><td class="mono">' + esc(c.shortName) + "</td><td>" + esc(c.module) + "</td><td>" + c.time.toFixed(2) + "s</td><td>" + c.assertions + "</td></tr>"
    ).join("");
}

function topCards(cases) {
    return [...cases].sort((a, b) => b.assertions - a.assertions).slice(0, 4).map(c =>
        '<div class="stat-card"><div class="label">' + esc(c.module) + '</div><div class="value blue">' + c.assertions + '</div><div style="margin-top:.5rem;font-size:.8rem;color:var(--muted)">' + esc(c.shortName) + '</div><div style="font-size:.75rem;color:var(--muted)">' + c.time.toFixed(2) + "s</div></div>"
    ).join("");
}

function failureRows(cases) {
    const failed = cases.filter(c => c.isFailure);
    if (failed.length === 0) return "";
    return failed.map(c =>
        '<tr class="fail"><td class="mono">' + esc(c.shortName) + "</td><td>" + esc(c.module) + "</td><td>" + c.time.toFixed(2) + "s</td><td>" + esc(c.failureDetail) + "</td></tr>"
    ).join("");
}

function caseRows(arr, max) {
    const slice = arr.slice(0, max);
    const extra = arr.length > max ? '<tr><td colspan="3" style="text-align:center;color:var(--muted);padding:1rem">... and ' + (arr.length - max) + " more tests</td></tr>" : "";
    return slice.map(c =>
        '<tr class="' + (c.isFailure ? "fail" : "") + '"><td class="mono">' + esc(c.shortName) + "</td><td>" + c.time.toFixed(3) + "s</td><td>" + c.assertions + "</td></tr>"
    ).join("") + extra;
}

function pieData(stats) {
    return Object.entries(stats).sort(([a], [b]) => a.localeCompare(b)).map(([mod, s]) =>
        '    "' + mod + '" : ' + s.tests
    ).join("\n");
}

function barData(stats) {
    const entries = Object.entries(stats).sort(([a], [b]) => a.localeCompare(b));
    return {
        axis: entries.map(([mod]) => '"' + mod + '"').join(", "),
        values: entries.map(([, s]) => s.time.toFixed(1)).join(", ")
    };
}

function generateHtml(allCases, titleText, commit, run) {
    const stats = buildStats(allCases);
    const total = allCases.length;
    const failed = allCases.filter(c => c.isFailure).length;
    const passed = total - failed;
    const tTime = totalTime(stats).toFixed(1);
    const tAsserts = totalAssertions(stats);
    const bar = barData(stats);

    const byMod = {};
    for (const c of allCases) {
        if (!byMod[c.module]) byMod[c.module] = [];
        byMod[c.module].push(c);
    }
    for (const mod of Object.keys(byMod)) {
        byMod[mod].sort((a, b) => b.time - a.time);
    }

    const modTables = Object.entries(byMod).sort(([a], [b]) => a.localeCompare(b)).map(([mod, cases]) => {
        const s = stats[mod];
        const maxShow = mod === "libslic3r" ? 30 : cases.length;
        return '\n    <h2>' + esc(mod) + " &mdash; " + s.tests + " Tests" + (s.failed ? ' <span style="color:var(--red)">(' + s.failed + " failed)</span>" : "") + '</h2>\n    <table>\n      <tr><th>Test</th><th>Time</th><th>Assertions</th></tr>\n      ' + caseRows(cases, maxShow) + '\n    </table>';
    }).join("");

    const failSection = failed > 0 ? '\n    <h2>Failed Tests</h2>\n    <table>\n      <tr><th>Test</th><th>Module</th><th>Time</th><th>Error</th></tr>\n      ' + failureRows(allCases) + '\n    </table>' : "";

    const slowSection = allCases.length > 0 ? '\n    <h2>Slowest 15 Tests</h2>\n    <table>\n      <tr><th>Test</th><th>Module</th><th>Time</th><th>Assertions</th></tr>\n      ' + slowRows(allCases, 15) + '\n    </table>' : "";

    const topSection = allCases.length > 0 ? '\n    <h2>Top Assertion-Heavy Tests</h2>\n    <div class="stat-grid">' + topCards(allCases) + '</div>' : "";

    const badgeHtml = failed === 0
        ? '<div class="pass-badge"><span class="dot"></span> ALL ' + total + ' TESTS PASSED</div>'
        : '<div class="fail-badge"><span class="dot"></span> ' + failed + ' OF ' + total + ' TESTS FAILED</div>';

    const subtitle = [];
    if (commit) subtitle.push('Commit <code>' + esc(commit.substring(0, 8)) + '</code>');
    if (run) {
        const runId = run.split("/").pop();
        subtitle.push('CI <a href="' + esc(run) + '">' + esc(runId) + '</a>');
    }
    subtitle.push(new Date().toISOString().split("T")[0]);

    return '<!DOCTYPE html>\n<html lang="en">\n<head>\n<meta charset="UTF-8">\n<meta name="viewport" content="width=device-width, initial-scale=1.0">\n<title>' + esc(titleText) + '</title>\n' + css() + '\n</head>\n<body>\n<div class="container">\n<h1>' + esc(titleText) + '</h1>\n<p class="subtitle">' + subtitle.join(" &middot; ") + '</p>\n' + badgeHtml + '\n\n<h2>Summary</h2>\n<div class="stat-grid">\n  <div class="stat-card"><div class="label">Total Tests</div><div class="value blue">' + total + '</div></div>\n  <div class="stat-card"><div class="label">Passed</div><div class="value green">' + passed + '</div></div>\n  <div class="stat-card"><div class="label">Failed / Errors</div><div class="value" style="color:' + (failed ? 'var(--red)' : 'var(--green)') + '">' + failed + '</div></div>\n  <div class="stat-card"><div class="label">Total Time</div><div class="value blue">' + tTime + 's</div></div>\n  <div class="stat-card"><div class="label">Assertions</div><div class="value blue">' + tAsserts + '</div></div>\n</div>\n' + (total > 0 ? '\n<h2>Module Breakdown</h2>\n<table>\n  <tr><th>Module</th><th>Tests</th><th>Time</th><th>Assertions</th><th>Failed</th></tr>\n  ' + modTable(stats) + '\n  <tr style="font-weight:600;background:var(--card)"><td>Total</td><td>' + total + '</td><td>' + tTime + 's</td><td>' + tAsserts + '</td><td>' + failed + '</td></tr>\n</table>\n\n<div class="mermaid-section">\n<pre class="mermaid">\npie showData\n    title Test Cases by Module (' + total + ' total)\n' + pieData(stats) + '\n</pre>\n</div>\n\n<div class="mermaid-section">\n<pre class="mermaid">\n%%{init: {"theme": "dark"}}%%\nxychart-beta\n    title "Execution Time by Module"\n    x-axis [' + bar.axis + ']\n    y-axis "Seconds" 0 --> ' + (Math.ceil(totalTime(stats)) + 1) + '\n    bar [' + bar.values + ']\n</pre>\n</div>\n' : '<div class="empty-state"><div class="icon">&#128269;</div><p>No test cases found in input.</p><p style="color:var(--muted);font-size:.85rem">Check that ctest --output-junit produced valid XML.</p></div>\n') + '\n' + failSection + '\n' + slowSection + '\n' + topSection + '\n' + modTables + '\n<footer>Generated ' + new Date().toISOString().replace("T", " ").substring(0, 19) + ' &middot; OrcaSlicer test suite</footer>\n</div>\n<script src="https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js"><\/script>\n<script>mermaid.initialize({startOnLoad:true,theme:"dark"});<\/script>\n</body>\n</html>';
}

function css() {
    return '<style>\n  :root{--bg:#0f1117;--card:#1a1d27;--border:#2a2d3a;--text:#c9d1d9;--muted:#8b949e;--green:#3fb950;--green-bg:rgba(63,185,80,0.1);--blue:#58a6ff;--red:#f85149;--red-bg:rgba(248,81,73,0.1);--purple:#a371f7}\n  *{margin:0;padding:0;box-sizing:border-box}\n  body{font-family:-apple-system,BlinkMacSystemFont,system-ui,sans-serif;background:var(--bg);color:var(--text);line-height:1.6;padding:2rem}\n  .container{max-width:1100px;margin:0 auto}\n  h1{font-size:1.75rem;font-weight:700;margin-bottom:.25rem}\n  h2{font-size:1.2rem;font-weight:600;margin:2.5rem 0 1rem;color:var(--blue);border-bottom:1px solid var(--border);padding-bottom:.5rem}\n  .subtitle{color:var(--muted);font-size:.875rem;margin-bottom:1.5rem}\n  .pass-badge{display:inline-flex;align-items:center;gap:.5rem;background:var(--green-bg);color:var(--green);padding:.5rem 1.25rem;border-radius:6px;font-weight:700;font-size:1.25rem;margin-bottom:1.5rem}\n  .pass-badge .dot{width:10px;height:10px;background:var(--green);border-radius:50%}\n  .fail-badge{display:inline-flex;align-items:center;gap:.5rem;background:var(--red-bg);color:var(--red);padding:.5rem 1.25rem;border-radius:6px;font-weight:700;font-size:1.25rem;margin-bottom:1.5rem}\n  .fail-badge .dot{width:10px;height:10px;background:var(--red);border-radius:50%}\n  .stat-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:1rem;margin-bottom:2rem}\n  .stat-card{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:1.25rem}\n  .stat-card .label{color:var(--muted);font-size:.8rem;text-transform:uppercase;letter-spacing:.05em}\n  .stat-card .value{font-size:1.75rem;font-weight:700;margin-top:.25rem}\n  .green{color:var(--green)}.blue{color:var(--blue)}\n  table{width:100%;border-collapse:collapse;margin-bottom:1.5rem;font-size:.875rem}\n  th,td{padding:.55rem .7rem;text-align:left;border-bottom:1px solid var(--border)}\n  th{color:var(--muted);font-size:.75rem;text-transform:uppercase;letter-spacing:.05em;font-weight:600}\n  tr:hover{background:rgba(255,255,255,.02)}\n  tr.fail{background:var(--red-bg)}\n  .mono{font-family:Cascadia Code,Fira Code,JetBrains Mono,monospace;font-size:.78rem}\n  .mermaid-section{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:1.5rem;margin-bottom:2rem;overflow-x:auto}\n  footer{text-align:center;color:var(--muted);font-size:.8rem;margin-top:3rem;padding-top:1.5rem;border-top:1px solid var(--border)}\n  a{color:var(--blue)}\n  .empty-state{text-align:center;padding:3rem;color:var(--muted)}\n</style>';
}

// ── Main ─────────────────────────────────────────────────────────────────────────
async function main() {
    const allCases = [];

    if (inputFiles.length === 0) {
        const chunks = [];
        for await (const chunk of process.stdin) {
            chunks.push(chunk);
        }
        const xml = Buffer.concat(chunks).toString("utf-8");
        if (xml.trim()) {
            allCases.push(...parseJUnit(xml));
        }
    } else {
        for (const file of inputFiles) {
            if (!fs.existsSync(file)) {
                console.error("File not found: " + file);
                process.exit(1);
            }
            const xml = fs.readFileSync(file, "utf-8");
            allCases.push(...parseJUnit(xml));
        }
    }

    const html = generateHtml(allCases, title, commitSha, runUrl);

    if (outputFile) {
        fs.mkdirSync(path.dirname(path.resolve(outputFile)), { recursive: true });
        fs.writeFileSync(outputFile, html, "utf-8");
        console.error("Wrote " + html.length + " bytes to " + outputFile);
    } else {
        process.stdout.write(html);
    }
}

main().catch(err => {
    console.error("Error: " + err.message);
    process.exit(1);
});

