// Client-side behavior for the mdy-built lamassu-js docs: theme toggle,
// mobile sidebar, local search over /search-index.json, code-block copy
// buttons + a tiny regex highlighter, heading anchor links, and an
// "On this page" scroll spy. Dependency-free.
(() => {
  const esc = (s) =>
    s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");

  // ---------- theme toggle ----------
  const toggle = document.getElementById("theme-toggle");
  if (toggle) {
    toggle.addEventListener("click", () => {
      const dark = document.documentElement.classList.toggle("dark");
      try {
        localStorage.setItem("mdy-docs-theme", dark ? "dark" : "light");
      } catch {}
    });
  }

  // ---------- mobile sidebar ----------
  const hamburger = document.getElementById("hamburger");
  const mask = document.getElementById("sidebar-mask");
  if (hamburger) {
    hamburger.addEventListener("click", () =>
      document.body.classList.toggle("sidebar-open")
    );
  }
  if (mask) {
    mask.addEventListener("click", () =>
      document.body.classList.remove("sidebar-open")
    );
  }

  // ---------- heading anchors ----------
  for (const h of document.querySelectorAll(
    ".doc-body h1[id], .doc-body h2[id], .doc-body h3[id], .doc-body h4[id]"
  )) {
    const a = document.createElement("a");
    a.className = "header-anchor";
    a.href = "#" + h.id;
    a.textContent = "#";
    a.setAttribute("aria-label", "Permalink");
    h.prepend(a);
  }

  // ---------- code blocks: wrap, label, copy, highlight ----------
  const KEYWORDS = {
    js: "async await break case catch class const continue default delete do else export extends finally for from function if import in instanceof let new null of return static switch this throw try typeof undefined var void while yield true false",
    c: "auto bool break case char const continue default do double else enum extern float for goto if int long register return short signed sizeof static struct switch typedef union unsigned void volatile while NULL true false size_t uint8_t uint32_t uint64_t int32_t int64_t",
    sh: "if then else elif fi for while do done case esac function echo export cd return exit local set",
  };
  KEYWORDS.ts = KEYWORDS.js + " interface type implements declare readonly namespace enum as satisfies";
  KEYWORDS.javascript = KEYWORDS.js;
  KEYWORDS.typescript = KEYWORDS.ts;
  KEYWORDS.bash = KEYWORDS.sh;
  KEYWORDS.shell = KEYWORDS.sh;

  const highlight = (code, lang) => {
    const kw = KEYWORDS[lang];
    if (!kw) return esc(code);
    const kwAlt = kw.trim().split(/\s+/).join("|");
    const comment =
      lang === "sh" || lang === "bash" || lang === "shell"
        ? "(#.*$)"
        : "(\\/\\/.*$|\\/\\*[\\s\\S]*?\\*\\/)";
    const re = new RegExp(
      comment +
        "|(\"(?:[^\"\\\\\\n]|\\\\.)*\"|'(?:[^'\\\\\\n]|\\\\.)*'|`(?:[^`\\\\]|\\\\.)*`)" +
        "|\\b(" + kwAlt + ")\\b" +
        "|(\\b0[xX][0-9a-fA-F]+\\b|\\b\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?\\b)",
      "gm"
    );
    let out = "";
    let last = 0;
    for (const m of code.matchAll(re)) {
      out += esc(code.slice(last, m.index));
      const cls = m[1] != null ? "tok-comment" : m[2] != null ? "tok-string" : m[3] != null ? "tok-keyword" : "tok-number";
      out += '<span class="' + cls + '">' + esc(m[0]) + "</span>";
      last = m.index + m[0].length;
    }
    return out + esc(code.slice(last));
  };

  for (const pre of document.querySelectorAll(".doc-body pre")) {
    const code = pre.querySelector("code");
    if (!code) continue;
    const langMatch = /language-(\w+)/.exec(code.className || "");
    const lang = langMatch ? langMatch[1] : "";
    const wrap = document.createElement("div");
    wrap.className = "code-block" + (lang ? " language-" + lang : "");
    pre.parentNode.insertBefore(wrap, pre);
    wrap.appendChild(pre);
    if (lang) {
      code.innerHTML = highlight(code.textContent, lang);
      const label = document.createElement("span");
      label.className = "code-lang";
      label.textContent = lang;
      wrap.appendChild(label);
    }
    const btn = document.createElement("button");
    btn.className = "code-copy";
    btn.setAttribute("aria-label", "Copy code");
    btn.innerHTML =
      '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>';
    btn.addEventListener("click", async () => {
      try {
        await navigator.clipboard.writeText(code.textContent);
        btn.classList.add("copied");
        setTimeout(() => btn.classList.remove("copied"), 1500);
      } catch {}
    });
    wrap.appendChild(btn);
  }

  // ---------- outline scroll spy ----------
  const outlineLinks = [...document.querySelectorAll(".outline-link")];
  if (outlineLinks.length > 0) {
    const targets = outlineLinks
      .map((a) => document.getElementById(decodeURIComponent(a.hash.slice(1))))
      .filter(Boolean);
    const setActive = () => {
      let current = targets[0];
      for (const t of targets) {
        if (t.getBoundingClientRect().top <= 96) current = t;
      }
      for (const a of outlineLinks) {
        a.classList.toggle(
          "active",
          current && decodeURIComponent(a.hash.slice(1)) === current.id
        );
      }
    };
    let ticking = false;
    addEventListener("scroll", () => {
      if (ticking) return;
      ticking = true;
      requestAnimationFrame(() => {
        setActive();
        ticking = false;
      });
    });
    setActive();
  }

  // ---------- local search ----------
  const input = document.getElementById("search-input");
  const results = document.getElementById("search-results");
  if (!input || !results) return;

  const STOP = new Set(
    "a an and are as at be but by for if in into is it no not of on or such that the their then there these they this to was will with".split(" ")
  );
  const tokenize = (text) => [
    ...new Set(
      String(text)
        .toLowerCase()
        .split(/[^a-z0-9]+/)
        .filter((w) => w.length > 1 && !STOP.has(w))
    ),
  ];

  let records = null;
  let selected = -1;
  const ready = () =>
    records
      ? Promise.resolve()
      : fetch("/search-index.json")
          .then((r) => r.json())
          .then((d) => (records = d));

  const close = () => {
    results.hidden = true;
    results.replaceChildren();
    selected = -1;
  };

  const render = (query) => {
    const tokens = tokenize(query);
    if (tokens.length === 0) return close();
    const scored = records
      .map((r) => {
        const words = r.words || [];
        const overlap = tokens.filter(
          (t) => words.includes(t) || words.some((w) => w.startsWith(t))
        ).length;
        const titleBoost = tokens.some((t) =>
          r.title.toLowerCase().includes(t)
        )
          ? 2
          : 0;
        return { r, score: overlap + titleBoost };
      })
      .filter(({ score }) => score > 0)
      .sort((a, b) => b.score - a.score)
      .slice(0, 8);
    selected = -1;
    if (scored.length === 0) {
      const li = document.createElement("li");
      li.className = "no-results";
      li.textContent = 'No results for "' + query + '"';
      results.replaceChildren(li);
    } else {
      results.replaceChildren(
        ...scored.map(({ r }) => {
          const li = document.createElement("li");
          const a = document.createElement("a");
          a.href = r.url;
          a.textContent = r.title;
          const ex = document.createElement("span");
          ex.className = "search-excerpt";
          ex.textContent = r.excerpt;
          a.appendChild(ex);
          li.appendChild(a);
          return li;
        })
      );
    }
    results.hidden = false;
  };

  let timer = null;
  input.addEventListener("input", () => {
    clearTimeout(timer);
    timer = setTimeout(async () => {
      await ready();
      render(input.value);
    }, 80);
  });
  input.addEventListener("keydown", (e) => {
    const items = [...results.querySelectorAll("li:not(.no-results)")];
    if (e.key === "Escape") {
      input.blur();
      close();
    } else if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      e.preventDefault();
      if (items.length === 0) return;
      selected =
        (selected + (e.key === "ArrowDown" ? 1 : -1) + items.length) %
        items.length;
      items.forEach((li, i) => li.classList.toggle("selected", i === selected));
    } else if (e.key === "Enter" && selected >= 0 && items[selected]) {
      location.href = items[selected].querySelector("a").href;
    }
  });
  document.addEventListener("click", (e) => {
    if (!e.target.closest("#search-box")) close();
  });
  document.addEventListener("keydown", (e) => {
    if (e.key === "/" && document.activeElement !== input) {
      e.preventDefault();
      input.focus();
    }
  });
})();
