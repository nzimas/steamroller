/* SPDX-License-Identifier: MIT -- SteamRoller website */
(function () {   /* narrow screens: MENU opens the navigation below the header */
  var btn = document.querySelector(".menu-btn"), nav = document.getElementById("site-nav");
  function set(open) {
    document.body.classList.toggle("menu-open", open);
    btn.setAttribute("aria-expanded", open ? "true" : "false");
    btn.textContent = open ? "CLOSE" : "MENU";
  }
  btn.addEventListener("click", function () { set(!document.body.classList.contains("menu-open")); });
  document.addEventListener("keydown", function (e) { if (e.key === "Escape") set(false); });
  nav.addEventListener("click", function (e) { if (e.target.tagName === "A") set(false); });
})();
