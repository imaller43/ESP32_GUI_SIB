(function () {
    const t = document.getElementById("themeToggle");
    const n = document.getElementById("navLogo");
    const c = document.getElementById("sidebar");
    const e = document.getElementById("toggleSidebar");
    function o(o) {
        if (t) t.innerHTML = o === "dark" ? '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"></path></svg>' : '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="5"></circle><line x1="12" y1="1" x2="12" y2="3"></line><line x1="12" y1="21" x2="12" y2="23"></line><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"></line><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"></line><line x1="1" y1="12" x2="3" y2="12"></line><line x1="21" y1="12" x2="23" y2="12"></line><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"></line><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"></line></svg>';
        if (n) n.src = o === "dark" ? "/SIB_logo.png" : "/SIB1_logo.png";
    }
    o(localStorage.getItem("theme") || "light");
    if (t) {
        t.addEventListener("click", function () {
            let t = document.documentElement.getAttribute("data-theme") === "dark" ? "light" : "dark";
            if (t === "dark") document.documentElement.setAttribute("data-theme", "dark");
            else document.documentElement.removeAttribute("data-theme");
            localStorage.setItem("theme", t);
            o(t);
        });
    }
    if (e && c) {
        e.addEventListener("click", () => c.classList.toggle("active"));
    }
})();
// Global utility functions
function escHtml(s) { return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;'); }
