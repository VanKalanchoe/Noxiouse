// Noxiouse Engine Documentation — Interactive Application Logic
// Powers the Godot / Read the Docs styled interface with zero installation.

(function() {
    "use strict";

    function safeGetTheme() {
        try {
            if (typeof window !== "undefined" && window.localStorage) {
                var stored = window.localStorage.getItem("nox-docs-theme");
                if (stored) return stored;
            }
        } catch (e) {
            // Ignore security error on file:/// URLs
        }
        return "dark";
    }

    function safeSetTheme(val) {
        try {
            if (typeof window !== "undefined" && window.localStorage) {
                window.localStorage.setItem("nox-docs-theme", val);
            }
        } catch (e) {
            // Ignore security error on file:/// URLs
        }
    }

    var state = {
        currentPageId: "index",
        searchQuery: "",
        theme: safeGetTheme(),
        activeCategoryPill: "All",
        fileSearchQuery: ""
    };

    // Apply theme immediately
    try {
        document.documentElement.setAttribute("data-theme", state.theme);
    } catch (e) {}

    // Simple Markdown to HTML parser
    function parseMarkdown(md) {
        if (!md) return "";

        var lines = Array.isArray(md) ? md : (typeof md === "string" ? md.split(/\r?\n/) : []);
        var html = [];
        var inCode = false;
        var codeLang = "";
        var codeLines = [];
        var inPipeline = false;
        var pipelineSteps = [];
        var inTable = false;
        var tableLines = [];
        var inAdmonition = false;
        var admonitionType = "";
        var admonitionTitle = "";
        var admonitionLines = [];

        function escapeHtml(text) {
            return text
                .replace(/&/g, "&amp;")
                .replace(/</g, "&lt;")
                .replace(/>/g, "&gt;")
                .replace(/"/g, "&quot;")
                .replace(/'/g, "&#039;");
        }

        function formatInline(text) {
            // file:/// links with custom text [text](file:///path)
            text = text.replace(/\[([^\]]+)\]\((file:\/\/\/[^\)]+)\)/g, function(match, label, url) {
                return '<a href="' + escapeHtml(url) + '" target="_blank" rel="noopener">' + label + '</a>';
            });

            // Standard markdown links [text](url)
            text = text.replace(/\[([^\]]+)\]\(([^)]+)\)/g, function(match, label, url) {
                if (url.endsWith('.md')) {
                    var pageId = url.replace(/\.md$/, "");
                    return '<a href="#' + pageId + '" class="internal-doc-link">' + label + '</a>';
                }
                return '<a href="' + escapeHtml(url) + '" target="_blank" rel="noopener">' + label + '</a>';
            });

            // Inline code `code`
            text = text.replace(/`([^`]+)`/g, function(match, code) {
                return '<code>' + escapeHtml(code) + '</code>';
            });

            // Bold **text**
            text = text.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');

            // Math expressions $expr$
            text = text.replace(/\$([^$]+)\$/g, '<em class="math-expr">$1</em>');

            // Italic *text*
            text = text.replace(/\*([^*]+)\*/g, '<em>$1</em>');

            return text;
        }

        function flushTable() {
            if (tableLines.length < 2) {
                tableLines = [];
                inTable = false;
                return;
            }

            var tHtml = '<div class="table-wrap"><table class="docutils"><thead><tr>';
            var headerCells = tableLines[0].split('|').map(function(c) { return c.trim(); }).filter(function(c, i, a) { return i > 0 && i < a.length - 1; });
            for (var h = 0; h < headerCells.length; h++) {
                tHtml += '<th>' + formatInline(headerCells[h]) + '</th>';
            }
            tHtml += '</tr></thead><tbody>';

            for (var r = 2; r < tableLines.length; r++) {
                var rowCells = tableLines[r].split('|').map(function(c) { return c.trim(); }).filter(function(c, i, a) { return i > 0 && i < a.length - 1; });
                if (rowCells.length === 0) continue;
                tHtml += '<tr>';
                for (var c = 0; c < rowCells.length; c++) {
                    tHtml += '<td>' + formatInline(rowCells[c]) + '</td>';
                }
                tHtml += '</tr>';
            }

            tHtml += '</tbody></table></div>';
            html.push(tHtml);
            tableLines = [];
            inTable = false;
        }

        function flushPipeline() {
            var pHtml = '<div class="pipeline-diagram">';
            for (var p = 0; p < pipelineSteps.length; p++) {
                var parts = pipelineSteps[p].split('|').map(function(s) { return s.trim(); });
                if (parts.length >= 2) {
                    pHtml += '<div class="pipeline-step">';
                    pHtml += '<div class="pipeline-step-num">' + escapeHtml(parts[0]) + '</div>';
                    pHtml += '<div class="pipeline-step-title">' + formatInline(parts[1]) + '</div>';
                    if (parts[2]) {
                        pHtml += '<div class="pipeline-step-desc">' + formatInline(parts[2]) + '</div>';
                    }
                    pHtml += '</div>';
                }
            }
            pHtml += '</div>';
            html.push(pHtml);
            pipelineSteps = [];
            inPipeline = false;
        }

        function flushAdmonition() {
            var aHtml = '<div class="admonition ' + escapeHtml(admonitionType) + '">';
            aHtml += '<div class="admonition-title">' + escapeHtml(admonitionTitle) + '</div>';
            aHtml += '<div class="admonition-body">';
            for (var a = 0; a < admonitionLines.length; a++) {
                aHtml += '<p>' + formatInline(admonitionLines[a]) + '</p>';
            }
            aHtml += '</div></div>';
            html.push(aHtml);
            admonitionLines = [];
            inAdmonition = false;
        }

        for (var i = 0; i < lines.length; i++) {
            var line = lines[i];

            // Code blocks
            if (line.indexOf("```pipeline") === 0) {
                inPipeline = true;
                pipelineSteps = [];
                continue;
            }
            if (inPipeline) {
                if (line.indexOf("```") === 0) {
                    flushPipeline();
                } else if (line.trim().length > 0) {
                    pipelineSteps.push(line.trim());
                }
                continue;
            }

            if (line.indexOf("```") === 0) {
                if (!inCode) {
                    inCode = true;
                    codeLang = line.substring(3).trim();
                    codeLines = [];
                } else {
                    inCode = false;
                    html.push('<div class="code-block-wrapper"><pre><code>' + escapeHtml(codeLines.join("\n")) + '</code></pre></div>');
                    codeLines = [];
                }
                continue;
            }
            if (inCode) {
                codeLines.push(line);
                continue;
            }

            // Admonitions (:::{tip}, :::{note}, :::{important}, :::)
            if (line.indexOf(":::{") === 0) {
                inAdmonition = true;
                admonitionType = line.substring(4, line.indexOf("}")).trim();
                admonitionTitle = admonitionType.toUpperCase();
                admonitionLines = [];
                continue;
            }
            if (inAdmonition) {
                if (line.indexOf(":::") === 0) {
                    flushAdmonition();
                } else if (line.trim().length > 0) {
                    admonitionLines.push(line);
                }
                continue;
            }

            // Markdown tables (| col | col |)
            if (line.trim().indexOf("|") === 0 && line.trim().lastIndexOf("|") === line.trim().length - 1) {
                inTable = true;
                tableLines.push(line);
                continue;
            } else if (inTable) {
                flushTable();
            }

            // Horizontal rule
            if (line.trim() === "---") {
                html.push("<hr>");
                continue;
            }

            // Headings
            if (line.indexOf("# ") === 0) {
                html.push("<h1>" + formatInline(line.substring(2)) + "</h1>");
                continue;
            }
            if (line.indexOf("## ") === 0) {
                var h2Text = line.substring(3);
                var h2Id = h2Text.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/(^-|-$)/g, "");
                html.push('<h2 id="' + h2Id + '">' + formatInline(h2Text) + '</h2>');
                continue;
            }
            if (line.indexOf("### ") === 0) {
                html.push("<h3>" + formatInline(line.substring(4)) + "</h3>");
                continue;
            }
            if (line.indexOf("#### ") === 0) {
                html.push("<h4>" + formatInline(line.substring(5)) + "</h4>");
                continue;
            }

            // Lists
            if (line.trim().indexOf("- ") === 0 || line.trim().indexOf("* ") === 0) {
                html.push("<ul><li>" + formatInline(line.trim().substring(2)) + "</li></ul>");
                continue;
            }
            var numMatch = line.trim().match(/^([0-9]+)\.\s+(.*)$/);
            if (numMatch) {
                html.push("<ol start=\"" + numMatch[1] + "\"><li>" + formatInline(numMatch[2]) + "</li></ol>");
                continue;
            }

            // Regular paragraph
            if (line.trim().length > 0) {
                html.push("<p>" + formatInline(line) + "</p>");
            }
        }

        if (inTable) flushTable();
        if (inAdmonition) flushAdmonition();

        return html.join("\n");
    }

    // Build the Left Navigation Menu
    function buildNavigation() {
        var menuEl = document.getElementById("rtd-menu");
        if (!menuEl) return;

        var pages = window.NOX_MANUAL_PAGES || window.NOX_DOCS_PAGES || [];
        var sections = {};
        for (var i = 0; i < pages.length; i++) {
            var p = pages[i];
            if (!sections[p.section]) sections[p.section] = [];
            sections[p.section].push(p);
        }

        var navHtml = "";
        for (var sName in sections) {
            navHtml += '<p class="wy-menu-caption">' + sName + '</p>';
            navHtml += '<ul class="nav-list">';
            var sPages = sections[sName];
            for (var j = 0; j < sPages.length; j++) {
                var page = sPages[j];
                var isActive = page.id === state.currentPageId;
                navHtml += '<li class="nav-item ' + (isActive ? "active" : "") + '">';
                navHtml += '<a href="#' + page.id + '">';
                navHtml += '<span>' + page.title + '</span>';
                if (page.badge) {
                    navHtml += '<span class="nav-badge">' + page.badge + '</span>';
                }
                navHtml += '</a>';
                navHtml += '</li>';
            }
            navHtml += '</ul>';
        }

        menuEl.innerHTML = navHtml;
    }

    // Render the Active Page
    function renderPage(pageId) {
        var pages = window.NOX_MANUAL_PAGES || window.NOX_DOCS_PAGES || [];
        var page = null;
        var pageIdx = 0;
        for (var i = 0; i < pages.length; i++) {
            if (pages[i].id === pageId) {
                page = pages[i];
                pageIdx = i;
                break;
            }
        }
        if (!page) {
            page = pages[0];
            pageIdx = 0;
            if (page) pageId = page.id;
        }
        if (!page) return;

        state.currentPageId = pageId;
        window.location.hash = pageId;
        document.title = page.title + " — Noxiouse Engine Documentation";

        // Update Breadcrumbs
        var breadcrumbsEl = document.getElementById("breadcrumb-trail");
        if (breadcrumbsEl) {
            breadcrumbsEl.innerHTML = '<a href="#index">Docs</a> &raquo; <span>' + page.section + '</span> &raquo; <span>' + page.title + '</span>';
        }

        // Render Markdown content
        var contentEl = document.getElementById("doc-content-body");
        if (contentEl) {
            contentEl.innerHTML = parseMarkdown(page.markdown);

            // Mount Master File Explorer if on the file-reference page
            if (pageId === "file-reference") {
                mountFileExplorer();
            }
        }

        // Update Prev / Next Buttons
        var prevBtn = document.getElementById("btn-prev-page");
        var nextBtn = document.getElementById("btn-next-page");
        if (prevBtn) {
            if (pageIdx > 0) {
                var prevPage = pages[pageIdx - 1];
                prevBtn.style.display = "inline-flex";
                prevBtn.href = "#" + prevPage.id;
                prevBtn.innerHTML = "&laquo; Previous (" + prevPage.title + ")";
            } else {
                prevBtn.style.display = "none";
            }
        }
        if (nextBtn) {
            if (pageIdx < pages.length - 1) {
                var nextPage = pages[pageIdx + 1];
                nextBtn.style.display = "inline-flex";
                nextBtn.href = "#" + nextPage.id;
                nextBtn.innerHTML = "Next (" + nextPage.title + ") &raquo;";
            } else {
                nextBtn.style.display = "none";
            }
        }

        buildNavigation();
        window.scrollTo(0, 0);
    }

    // Interactive File Explorer for the file-reference page
    function mountFileExplorer() {
        var mount = document.getElementById("master-file-explorer-mount");
        if (!mount) return;

        var files = window.NOX_FILE_DATABASE || [];
        var categories = ["All", "Root & Build", "NRI Graphics", "Vulkan 1.4", "Render Graph", "Renderer", "Renderer Passes", "Shaders", "Core & Engine", "Tasks & Workers", "Profiling", "Scene & ECS", "Assets & Streaming", "Physics", "Animation", "NodeGraph", "Scripting C++", "NoxScriptCore C#", "Editor & Runtime", "Editor Panels", "Sample Project", "Scripts & Docs"];

        var controlsHtml = '<div class="explorer-controls">';
        controlsHtml += '<input type="text" id="file-filter-input" class="explorer-search" placeholder="Filter 489 files by name, path, class, or keyword..." value="' + (state.fileSearchQuery || "") + '">';
        controlsHtml += '<div class="category-pills" id="category-pills-bar">';
        for (var c = 0; c < categories.length; c++) {
            var cat = categories[c];
            var isAct = cat === state.activeCategoryPill;
            controlsHtml += '<button class="cat-pill ' + (isAct ? "active" : "") + '" data-cat="' + cat + '">' + cat + '</button>';
        }
        controlsHtml += '</div></div>';
        controlsHtml += '<div id="file-results-container"></div>';

        mount.innerHTML = controlsHtml;

        function updateResults() {
            var query = (state.fileSearchQuery || "").toLowerCase();
            var activeCat = state.activeCategoryPill;
            var filtered = [];

            for (var f = 0; f < files.length; f++) {
                var file = files[f];
                if (activeCat !== "All" && file.category !== activeCat) continue;
                if (query.length > 0) {
                    var match = (file.path.toLowerCase().indexOf(query) !== -1) ||
                                (file.desc.toLowerCase().indexOf(query) !== -1) ||
                                (file.category.toLowerCase().indexOf(query) !== -1) ||
                                (file.type.toLowerCase().indexOf(query) !== -1);
                    if (!match) continue;
                }
                filtered.push(file);
            }

            var resEl = document.getElementById("file-results-container");
            if (!resEl) return;

            var rHtml = '<p style="margin-bottom:12px; font-size:0.9rem; color:var(--text-muted);">Showing <strong>' + filtered.length + '</strong> of ' + files.length + ' repository files</p>';
            rHtml += '<div class="table-wrap"><table class="docutils"><thead><tr>';
            rHtml += '<th style="width:38%;">File Path</th><th style="width:16%;">Category</th><th style="width:14%;">Type</th><th>Plain-English Summary</th>';
            rHtml += '</tr></thead><tbody>';

            for (var i = 0; i < filtered.length; i++) {
                var it = filtered[i];
                rHtml += '<tr>';
                rHtml += '<td><a href="file:///E:/dev/noxiouse/' + it.path + '" target="_blank" rel="noopener"><code>' + it.path + '</code></a></td>';
                rHtml += '<td><span class="cat-tag">' + it.category + '</span></td>';
                rHtml += '<td>' + it.type + '</td>';
                rHtml += '<td>' + it.desc + '</td>';
                rHtml += '</tr>';
            }

            rHtml += '</tbody></table></div>';
            resEl.innerHTML = rHtml;
        }

        // Attach event listeners
        var filterInput = document.getElementById("file-filter-input");
        if (filterInput) {
            filterInput.addEventListener("input", function(e) {
                state.fileSearchQuery = e.target.value;
                updateResults();
            });
        }

        var pillsBar = document.getElementById("category-pills-bar");
        if (pillsBar) {
            pillsBar.addEventListener("click", function(e) {
                var pill = e.target.closest(".cat-pill");
                if (pill) {
                    state.activeCategoryPill = pill.getAttribute("data-cat");
                    var allPills = pillsBar.querySelectorAll(".cat-pill");
                    allPills.forEach(function(p) { p.classList.remove("active"); });
                    pill.classList.add("active");
                    updateResults();
                }
            });
        }

        updateResults();
    }

    // Global Search across documentation pages and files
    function setupGlobalSearch() {
        var searchInput = document.getElementById("global-search-input");
        if (!searchInput) return;

        searchInput.addEventListener("input", function(e) {
            var q = e.target.value.trim().toLowerCase();
            if (q.length === 0) {
                // Restore current page
                renderPage(state.currentPageId);
                return;
            }

            var pages = window.NOX_MANUAL_PAGES || window.NOX_DOCS_PAGES || [];
            var files = window.NOX_FILE_DATABASE || [];
            var matchedPages = [];
            var matchedFiles = [];

            for (var p = 0; p < pages.length; p++) {
                var pg = pages[p];
                var mdText = Array.isArray(pg.markdown) ? pg.markdown.join(" ") : (pg.markdown || "");
                if (pg.title.toLowerCase().indexOf(q) !== -1 || mdText.toLowerCase().indexOf(q) !== -1) {
                    matchedPages.push(pg);
                }
            }

            for (var f = 0; f < files.length; f++) {
                var fl = files[f];
                if (fl.path.toLowerCase().indexOf(q) !== -1 || fl.desc.toLowerCase().indexOf(q) !== -1) {
                    matchedFiles.push(fl);
                }
            }

            var contentEl = document.getElementById("doc-content-body");
            if (!contentEl) return;

            var sHtml = '<h1>Search Results for "' + q + '"</h1>';
            sHtml += '<p style="color:var(--text-muted); margin-bottom:20px;">Found <strong>' + matchedPages.length + '</strong> documentation sections and <strong>' + matchedFiles.length + '</strong> repository files.</p>';

            if (matchedPages.length > 0) {
                sHtml += '<h2>Documentation Sections</h2><ul style="margin-bottom:24px;">';
                for (var i = 0; i < matchedPages.length; i++) {
                    var mp = matchedPages[i];
                    sHtml += '<li style="margin-bottom:8px;"><a href="#' + mp.id + '" style="font-size:1.05rem; font-weight:700;">' + mp.title + '</a> <span class="nav-badge">' + mp.section + '</span></li>';
                }
                sHtml += '</ul>';
            }

            if (matchedFiles.length > 0) {
                sHtml += '<h2>Matching Repository Files</h2>';
                sHtml += '<div class="table-wrap"><table class="docutils"><thead><tr><th>File Path</th><th>Category</th><th>Description</th></tr></thead><tbody>';
                for (var j = 0; j < Math.min(matchedFiles.length, 50); j++) {
                    var mf = matchedFiles[j];
                    sHtml += '<tr><td><a href="file:///E:/dev/noxiouse/' + mf.path + '" target="_blank"><code>' + mf.path + '</code></a></td><td><span class="cat-tag">' + mf.category + '</span></td><td>' + mf.desc + '</td></tr>';
                }
                sHtml += '</tbody></table></div>';
                if (matchedFiles.length > 50) {
                    sHtml += '<p style="font-size:0.85rem; color:var(--text-muted);">...and ' + (matchedFiles.length - 50) + ' more files. Use the <a href="#file-reference">Interactive 489-File Master Explorer</a> for full filtering.</p>';
                }
            }

            contentEl.innerHTML = sHtml;
        });
    }

    // Theme Toggle (Dark / Light)
    function setupThemeToggle() {
        var btn = document.getElementById("theme-toggle-btn");
        if (!btn) return;

        function updateBtnText() {
            btn.innerHTML = state.theme === "dark" ? "&#9728; Light Mode" : "&#9790; Dark Mode";
        }
        updateBtnText();

        btn.addEventListener("click", function() {
            state.theme = state.theme === "dark" ? "light" : "dark";
            try {
                document.documentElement.setAttribute("data-theme", state.theme);
            } catch (e) {}
            safeSetTheme(state.theme);
            updateBtnText();
        });
    }

    // Initialize App on DOM Ready
    function init() {
        try {
            buildNavigation();
            setupGlobalSearch();
            setupThemeToggle();

            // Check hash or default to first page
            var pages = window.NOX_MANUAL_PAGES || window.NOX_DOCS_PAGES || [];
            var defaultPageId = pages.length > 0 ? pages[0].id : "index";
            var initialHash = window.location.hash.replace(/^#/, "");
            renderPage(initialHash || defaultPageId);

            window.addEventListener("hashchange", function() {
                var newHash = window.location.hash.replace(/^#/, "");
                if (newHash && newHash !== state.currentPageId) {
                    renderPage(newHash);
                }
            });
        } catch (err) {
            console.error("NoxDocs initialization error:", err);
        }
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", init);
    } else {
        init();
    }
})();
