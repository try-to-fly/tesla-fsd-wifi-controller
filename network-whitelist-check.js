#!/usr/bin/env node
"use strict";

const dns = require("node:dns/promises");
const fs = require("node:fs/promises");
const https = require("node:https");
const path = require("node:path");

const DEFAULT_WHITELIST = [
    "baidu.com",
    "bdimg.com",
    "bdstatic.com",
    "api-prd.vn.cloud.tesla.cn",
    "connman.vn.cloud.tesla.cn",
    "nav-prd-maps.tesla.cn",
    "www.tesla.cn",
];

const DEFAULT_REFERENCE_SITES = [
    "qq.com",
    "taobao.com",
    "jd.com",
    "weibo.com",
    "bilibili.com",
    "zhihu.com",
    "sina.com.cn",
    "163.com",
];

const DEFAULT_OPTIONS = {
    mode: "restricted",
    timeoutMs: 5000,
    concurrency: 6,
    configFile: "",
    teslaCnExpected: "block",
};

const ANSI = {
    reset: "\x1b[0m",
    bold: "\x1b[1m",
    dim: "\x1b[2m",
    red: "\x1b[31m",
    green: "\x1b[32m",
    yellow: "\x1b[33m",
    blue: "\x1b[34m",
    cyan: "\x1b[36m",
};

function colorize(text, colorCode) {
    return `${colorCode}${text}${ANSI.reset}`;
}

function printHelp() {
    const text = `
Usage:
  node network-whitelist-check.js [options]

Options:
  --mode <restricted|open|observe>    Default: restricted
  --timeout <ms>                      Default: 5000
  --concurrency <n>                   Default: 6
  --tesla-cn-expected <allow|block|ignore>
                                      Default: block
  --config <path>                     Optional JSON config
  --help                              Show this help

Config JSON example:
{
  "whitelist": ["baidu.com"],
  "referenceSites": ["qq.com", "taobao.com"]
}

Mode behavior:
  restricted: whitelist should be accessible; tesla.cn(default) + reference sites should be blocked
  open:       whitelist + tesla.cn + reference sites should be accessible
  observe:    only report results, no PASS/FAIL expectation for reference sites
`.trim();
    console.log(text);
}

function parseArgs(argv) {
    const options = { ...DEFAULT_OPTIONS };

    for (let i = 0; i < argv.length; i += 1) {
        const arg = argv[i];

        if (arg === "--help" || arg === "-h") {
            options.help = true;
            continue;
        }

        if (arg === "--mode") {
            options.mode = argv[i + 1] || "";
            i += 1;
            continue;
        }

        if (arg.startsWith("--mode=")) {
            options.mode = arg.split("=")[1] || "";
            continue;
        }

        if (arg === "--timeout") {
            options.timeoutMs = Number(argv[i + 1]);
            i += 1;
            continue;
        }

        if (arg.startsWith("--timeout=")) {
            options.timeoutMs = Number(arg.split("=")[1]);
            continue;
        }

        if (arg === "--concurrency") {
            options.concurrency = Number(argv[i + 1]);
            i += 1;
            continue;
        }

        if (arg.startsWith("--concurrency=")) {
            options.concurrency = Number(arg.split("=")[1]);
            continue;
        }

        if (arg === "--config") {
            options.configFile = argv[i + 1] || "";
            i += 1;
            continue;
        }

        if (arg.startsWith("--config=")) {
            options.configFile = arg.split("=")[1] || "";
            continue;
        }

        if (arg === "--tesla-cn-expected") {
            options.teslaCnExpected = argv[i + 1] || "";
            i += 1;
            continue;
        }

        if (arg.startsWith("--tesla-cn-expected=")) {
            options.teslaCnExpected = arg.split("=")[1] || "";
            continue;
        }

        throw new Error(`Unknown argument: ${arg}`);
    }

    const validModes = new Set(["restricted", "open", "observe"]);
    if (!validModes.has(options.mode)) {
        throw new Error(`Invalid --mode: ${options.mode}`);
    }

    const validTeslaExpected = new Set(["allow", "block", "ignore"]);
    if (!validTeslaExpected.has(options.teslaCnExpected)) {
        throw new Error(
            `Invalid --tesla-cn-expected: ${options.teslaCnExpected}`,
        );
    }

    if (!Number.isFinite(options.timeoutMs) || options.timeoutMs < 1000) {
        throw new Error("Invalid --timeout, should be >= 1000 ms");
    }

    if (
        !Number.isFinite(options.concurrency) ||
        options.concurrency < 1 ||
        options.concurrency > 32
    ) {
        throw new Error("Invalid --concurrency, should be between 1 and 32");
    }

    return options;
}

function cleanDomainList(input, fallback) {
    const source = Array.isArray(input) ? input : fallback;
    const seen = new Set();
    const output = [];

    for (const item of source) {
        if (typeof item !== "string") {
            continue;
        }
        const d = item.trim().toLowerCase();
        if (!d) {
            continue;
        }
        if (!/^[a-z0-9.-]+$/.test(d)) {
            continue;
        }
        if (seen.has(d)) {
            continue;
        }
        seen.add(d);
        output.push(d);
    }
    return output;
}

async function loadConfig(configFile) {
    if (!configFile) {
        return {
            whitelist: [...DEFAULT_WHITELIST],
            referenceSites: [...DEFAULT_REFERENCE_SITES],
        };
    }

    const fullPath = path.resolve(configFile);
    const content = await fs.readFile(fullPath, "utf8");
    const parsed = JSON.parse(content);

    return {
        whitelist: cleanDomainList(parsed.whitelist, DEFAULT_WHITELIST),
        referenceSites: cleanDomainList(
            parsed.referenceSites,
            DEFAULT_REFERENCE_SITES,
        ),
    };
}

function withTimeout(promise, timeoutMs, timeoutCode) {
    let timer = null;
    return Promise.race([
        promise,
        new Promise((_, reject) => {
            timer = setTimeout(() => {
                const err = new Error(timeoutCode);
                err.code = timeoutCode;
                reject(err);
            }, timeoutMs);
        }),
    ]).finally(() => {
        if (timer) {
            clearTimeout(timer);
        }
    });
}

async function checkDns(domain, timeoutMs) {
    const start = Date.now();
    try {
        const addresses = await withTimeout(
            dns.lookup(domain, { all: true }),
            timeoutMs,
            "DNS_TIMEOUT",
        );
        return {
            ok: Array.isArray(addresses) && addresses.length > 0,
            addresses: Array.isArray(addresses)
                ? addresses.map((x) => x.address)
                : [],
            error: "",
            durationMs: Date.now() - start,
        };
    } catch (err) {
        return {
            ok: false,
            addresses: [],
            error: err.code || err.message || "DNS_ERROR",
            durationMs: Date.now() - start,
        };
    }
}

function isLikelyTlsCertError(code) {
    if (!code) {
        return false;
    }
    if (code.startsWith("ERR_TLS_")) {
        return true;
    }
    if (code.includes("CERT")) {
        return true;
    }
    const known = new Set([
        "SELF_SIGNED_CERT_IN_CHAIN",
        "UNABLE_TO_VERIFY_LEAF_SIGNATURE",
        "DEPTH_ZERO_SELF_SIGNED_CERT",
    ]);
    return known.has(code);
}

function requestHttpsHead({ domain, timeoutMs, rejectUnauthorized }) {
    const start = Date.now();

    return new Promise((resolve) => {
        const req = https.request(
            {
                protocol: "https:",
                hostname: domain,
                port: 443,
                method: "HEAD",
                path: "/",
                timeout: timeoutMs,
                rejectUnauthorized,
                servername: domain,
                headers: {
                    "User-Agent": "network-whitelist-check/1.0",
                    Accept: "*/*",
                },
            },
            (res) => {
                resolve({
                    ok: true,
                    statusCode: res.statusCode || 0,
                    error: "",
                    transport: "https",
                    tlsMode: rejectUnauthorized ? "strict" : "insecure",
                    durationMs: Date.now() - start,
                });
            },
        );

        req.on("timeout", () => {
            req.destroy(
                Object.assign(new Error("HTTPS_TIMEOUT"), {
                    code: "HTTPS_TIMEOUT",
                }),
            );
        });

        req.on("error", (err) => {
            resolve({
                ok: false,
                statusCode: 0,
                error: err.code || err.message || "HTTPS_ERROR",
                transport: "https",
                tlsMode: rejectUnauthorized ? "strict" : "insecure",
                durationMs: Date.now() - start,
            });
        });

        req.end();
    });
}

async function checkAccess(domain, timeoutMs) {
    const strictHttps = await requestHttpsHead({
        domain,
        timeoutMs,
        rejectUnauthorized: true,
    });
    if (strictHttps.ok) {
        return strictHttps;
    }

    if (isLikelyTlsCertError(strictHttps.error)) {
        const insecureHttps = await requestHttpsHead({
            domain,
            timeoutMs,
            rejectUnauthorized: false,
        });
        if (insecureHttps.ok) {
            return {
                ...insecureHttps,
                note: `TLS cert issue ignored: ${strictHttps.error}`,
            };
        }
    }

    return strictHttps;
}

function expectedAccessByMode(mode, group) {
    if (mode === "restricted") {
        if (group === "reference") {
            return false;
        }
        return true;
    }
    if (mode === "open") {
        return true;
    }
    return null;
}

function decidePass(expectedAccess, dnsResult, httpsResult) {
    if (expectedAccess === null) {
        return null;
    }
    if (expectedAccess === true) {
        return dnsResult.ok && httpsResult.ok;
    }
    return httpsResult.ok === false;
}

async function checkOne(domain, group, expectedAccess, timeoutMs) {
    const dnsTimeoutMs =
        expectedAccess === false ? Math.min(timeoutMs, 2500) : timeoutMs;
    const accessTimeoutMs =
        expectedAccess === false ? Math.min(timeoutMs, 3500) : timeoutMs;
    const [dnsResult, httpsResult] = await Promise.all([
        checkDns(domain, dnsTimeoutMs),
        checkAccess(domain, accessTimeoutMs),
    ]);

    return {
        domain,
        group,
        expectedAccess,
        dns: dnsResult,
        https: httpsResult,
        pass: decidePass(expectedAccess, dnsResult, httpsResult),
    };
}

function pad(input, length) {
    const s = String(input);
    if (s.length >= length) {
        return s;
    }
    return s + " ".repeat(length - s.length);
}

function stringifyExpected(value) {
    if (value === true) {
        return "ALLOW";
    }
    if (value === false) {
        return "BLOCK";
    }
    return "N/A";
}

function stringifyVerdict(value) {
    if (value === true) {
        return "PASS";
    }
    if (value === false) {
        return "FAIL";
    }
    return "N/A";
}

function stringifyDns(result) {
    if (result.ok) {
        return `OK(${result.addresses.length})`;
    }
    return `FAIL(${result.error})`;
}

function stringifyHttps(result) {
    if (result.ok) {
        return `OK(${result.statusCode},${result.tlsMode})`;
    }
    return `FAIL(${result.error})`;
}

function colorExpected(raw, paddedText) {
    if (raw === "ALLOW") {
        return colorize(paddedText, `${ANSI.bold}${ANSI.cyan}`);
    }
    if (raw === "BLOCK") {
        return colorize(paddedText, `${ANSI.bold}${ANSI.yellow}`);
    }
    return colorize(paddedText, ANSI.dim);
}

function colorStatus(text) {
    if (text.startsWith("OK")) {
        return colorize(text, ANSI.green);
    }
    return colorize(text, ANSI.red);
}

function colorVerdict(raw, paddedText) {
    if (raw === "PASS") {
        return colorize(paddedText, `${ANSI.bold}${ANSI.green}`);
    }
    if (raw === "FAIL") {
        return colorize(paddedText, `${ANSI.bold}${ANSI.red}`);
    }
    return colorize(paddedText, ANSI.dim);
}

function printResults(results, options, config) {
    console.log("");
    console.log(
        colorize("=== Network Domain Check ===", `${ANSI.bold}${ANSI.blue}`),
    );
    console.log(`Mode: ${colorize(options.mode, ANSI.cyan)}`);
    console.log(`Timeout: ${colorize(`${options.timeoutMs}ms`, ANSI.cyan)}`);
    console.log(
        `Concurrency: ${colorize(String(options.concurrency), ANSI.cyan)}`,
    );
    console.log(
        `Whitelist: ${colorize(config.whitelist.join(", "), ANSI.cyan)}`,
    );
    console.log(
        `Reference sites: ${colorize(config.referenceSites.join(", "), ANSI.cyan)}`,
    );
    console.log("");

    const headers = [
        pad("Domain", 30),
        pad("Group", 10),
        pad("Expected", 8),
        pad("DNS", 22),
        pad("HTTPS", 22),
        pad("Verdict", 8),
    ];

    console.log(headers.join(" | "));
    console.log("-".repeat(headers.join(" | ").length));

    for (const r of results) {
        const expectedRaw = stringifyExpected(r.expectedAccess);
        const dnsRaw = stringifyDns(r.dns);
        const httpsRaw = stringifyHttps(r.https);
        const verdictRaw = stringifyVerdict(r.pass);

        const row = [
            pad(r.domain, 30),
            pad(r.group, 10),
            colorExpected(expectedRaw, pad(expectedRaw, 8)),
            colorStatus(pad(dnsRaw, 22)),
            colorStatus(pad(httpsRaw, 22)),
            colorVerdict(verdictRaw, pad(verdictRaw, 8)),
        ];
        console.log(row.join(" | "));
    }

    const evaluated = results.filter((r) => r.pass !== null);
    const passed = evaluated.filter((r) => r.pass === true).length;
    const failed = evaluated.filter((r) => r.pass === false).length;
    const unresolved = results.filter((r) => r.pass === null).length;

    console.log("");
    console.log(colorize("Summary:", `${ANSI.bold}${ANSI.blue}`));
    console.log(
        `  evaluated=${evaluated.length}, pass=${passed}, fail=${failed}, no_expectation=${unresolved}`,
    );

    if (failed > 0) {
        console.log(`  overall=${colorize("FAIL", `${ANSI.bold}${ANSI.red}`)}`);
    } else if (evaluated.length === 0) {
        console.log(`  overall=${colorize("N/A", ANSI.dim)}`);
    } else {
        console.log(
            `  overall=${colorize("PASS", `${ANSI.bold}${ANSI.green}`)}`,
        );
    }
}

function printProgress(result, doneCount, totalCount, durationMs) {
    const verdictRaw = stringifyVerdict(result.pass);
    const dnsRaw = stringifyDns(result.dns);
    const httpsRaw = stringifyHttps(result.https);
    const expectedRaw = stringifyExpected(result.expectedAccess);

    const header = colorize(
        `[${doneCount}/${totalCount}] ${result.domain} (${result.group})`,
        ANSI.dim,
    );

    const expectedText = colorExpected(expectedRaw, expectedRaw);
    const verdictText = colorVerdict(verdictRaw, verdictRaw);
    const dnsText = colorStatus(dnsRaw);
    const httpsText = colorStatus(httpsRaw);
    const timeText = colorize(`${durationMs}ms`, ANSI.dim);

    console.log(
        `${header} expected=${expectedText} verdict=${verdictText} dns=${dnsText} https=${httpsText} time=${timeText}`,
    );
}

function printProgressStart(check, index, totalCount) {
    const expectedRaw = stringifyExpected(check.expectedAccess);
    const expectedText = colorExpected(expectedRaw, expectedRaw);
    console.log(
        colorize(
            `[${index + 1}/${totalCount}] START ${check.domain} (${check.group}) expected=${expectedText}`,
            ANSI.dim,
        ),
    );
}

async function runChecksWithConcurrency(checks, options) {
    const total = checks.length;
    if (total === 0) {
        return [];
    }

    const concurrency = Math.min(options.concurrency, total);
    const results = new Array(total);
    let nextIndex = 0;
    let done = 0;

    const workers = Array.from({ length: concurrency }, async () => {
        while (true) {
            const index = nextIndex;
            nextIndex += 1;
            if (index >= total) {
                return;
            }

            const item = checks[index];
            printProgressStart(item, index, total);
            const startedAt = Date.now();
            const result = await checkOne(
                item.domain,
                item.group,
                item.expectedAccess,
                options.timeoutMs,
            );

            results[index] = result;
            done += 1;
            printProgress(result, done, total, Date.now() - startedAt);
        }
    });

    await Promise.all(workers);
    return results;
}

async function main() {
    try {
        const options = parseArgs(process.argv.slice(2));
        if (options.help) {
            printHelp();
            return;
        }

        const config = await loadConfig(options.configFile);
        const checks = [];

        for (const domain of config.whitelist) {
            checks.push({
                domain,
                group: "whitelist",
                expectedAccess: expectedAccessByMode(options.mode, "whitelist"),
            });
        }

        if (options.teslaCnExpected !== "ignore") {
            checks.push({
                domain: "tesla.cn",
                group: "tesla",
                expectedAccess: options.teslaCnExpected === "allow",
            });
        }

        for (const domain of config.referenceSites) {
            checks.push({
                domain,
                group: "reference",
                expectedAccess: expectedAccessByMode(options.mode, "reference"),
            });
        }

        console.log(
            colorize(
                `开始检测，共 ${checks.length} 个域名（并发 ${Math.min(
                    options.concurrency,
                    checks.length,
                )}）...`,
                `${ANSI.bold}${ANSI.blue}`,
            ),
        );
        const batchStartAt = Date.now();
        const results = await runChecksWithConcurrency(checks, options);
        console.log(
            colorize(
                `检测完成，用时 ${Date.now() - batchStartAt}ms`,
                `${ANSI.bold}${ANSI.blue}`,
            ),
        );

        printResults(results, options, config);

        const hasFailure = results.some((r) => r.pass === false);
        process.exitCode = hasFailure ? 2 : 0;
    } catch (err) {
        console.error(`Error: ${err.message || err}`);
        process.exitCode = 1;
    }
}

main();
