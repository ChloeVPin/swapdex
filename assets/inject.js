(() => {
  "use strict";

  if (window.__swapdexInstalled === true) {
    return;
  }

  Object.defineProperty(window, "__swapdexInstalled", {
    value: true,
    configurable: false,
    enumerable: false,
    writable: false
  });

  const language = document.documentElement.lang || "en";
  const supportedLanguage = language === "en" || language.startsWith("en-");
  const settingsLabel = "Settings";
  const logOutLabel = "Log out";
  const triggerLabels = new Set(["Open profile menu", "Open settings"]);
  const avatarStoragePrefix = "swapdex.avatar.v1.";
  const privacyStorageKey = "swapdex.privacy.blurProfileNames.v1";
  const maintenanceEnabledStorageKey = "swapdex.maintenance.enabled.v1";
  const maintenanceIntervalStorageKey = "swapdex.maintenance.intervalHours.v1";
  const preferenceStorageKey = "swapdex.preferences.v2";
  const optionalMenuItems = [
    { id: "usage", labels: ["Usage remaining", "Usage"], title: "Usage remaining", description: "Show your current usage limits in the profile menu." },
    { id: "upgrade", labels: ["Upgrade for higher limits", "Rejoin Plus"], title: "Upgrade", description: "Show upgrade or plan-rejoin actions when available." },
    { id: "workspace-settings", labels: ["Workspace settings"], title: "Workspace settings", description: "Show workspace-specific settings when available." },
    { id: "profile", labels: ["Profile"], title: "Profile", description: "Show the profile shortcut when available." },
    { id: "cme", labels: ["CME"], title: "CME", description: "Show the CME action when available." },
    { id: "invite", labels: ["Invite a friend", "Invite a coworker"], title: "Invite", description: "Show the invitation action when available." },
    { id: "pet", labels: ["Show pet", "Hide pet"], title: "Pet", description: "Show the pet action when available." },
    { id: "logout", labels: ["Log out"], title: "Log out", description: "Show the native sign-out action." }
  ];
  const optionalMenuIds = new Set(optionalMenuItems.map(item => item.id));
  const settingsPageId = "swapdex-settings-page";
  const menuState = new WeakMap();
  const nativeMenuRowState = new WeakMap();
  const activatedRows = new WeakSet();
  const pendingRequests = [];
  const settingsState = {
    active: false,
    tab: "privacy",
    nativePage: null,
    scroll: null,
    navButton: null,
    page: null,
    accentColor: "",
    offColor: "",
    thumbColor: "",
    idleClass: "",
    selectedClass: ""
  };
  let blurProfileNames = false;
  let maintenanceEnabled = true;
  let maintenanceIntervalHours = 12;
  let hiddenNativeProfileMenuRows = new Set();
  let snapshot = { active: "", profiles: [] };
  let renderGeneration = 0;
  let scheduled = false;
  let pendingStatus = "";
  let statusTimer = null;
  let preferenceSaveState = "saved";
  let intervalMenu = null;
  let intervalMenuButton = null;
  const pendingWarmups = new Map();
  const pendingRemovals = new Set();
  let confirmationDialog = null;

  const dialogButtonClasses = "no-drag cursor-interaction items-center select-none focus:outline-hidden disabled:cursor-default aria-disabled:cursor-default disabled:opacity-40 aria-disabled:opacity-40 focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-0 border gap-1 whitespace-nowrap flex rounded-button-action border-transparent px-4 py-1.5 text-base leading-[18px]";

  const destroyConfirmation = () => {
    if (!confirmationDialog) {
      return;
    }
    const current = confirmationDialog;
    confirmationDialog = null;
    window.removeEventListener("keydown", current.onKeyDown, true);
    current.surface.remove();
    current.overlay.remove();
    if (current.restoreFocus instanceof HTMLElement && current.restoreFocus.isConnected) {
      current.restoreFocus.focus({ preventScroll: true });
    }
  };

  const openConfirmation = options => {
    destroyConfirmation();
    const settings = typeof options === "object" && options !== null ? options : {};
    const restoreFocus = document.activeElement;
    const overlay = makeElement("div", "codex-dialog-overlay fixed inset-0 z-50 extension:bg-surface-tertiary/80 electron:bg-[#00000022] codex-dialog-overlay");
    overlay.dataset.swapdexConfirmOverlay = "true";
    overlay.setAttribute("data-state", "open");
    overlay.setAttribute("aria-hidden", "true");
    overlay.style.zIndex = "70";
    const surface = makeElement("div", "codex-dialog z-50 outline-none left-1/2 top-1/2 -translate-x-1/2 -translate-y-1/2 fixed bg-surface-elevated-secondary/90 text-default ring-border ring-[0.5px] ring-border shadow-lg backdrop-blur-xl rounded-3xl max-w-[92vw] overflow-hidden w-[420px]");
    surface.dataset.swapdexConfirmDialog = "true";
    surface.setAttribute("role", "alertdialog");
    surface.setAttribute("aria-modal", "true");
    surface.setAttribute("data-state", "open");
    surface.tabIndex = -1;
    surface.style.zIndex = "71";
    const titleId = "swapdex-confirm-title";
    const descriptionId = "swapdex-confirm-description";
    surface.setAttribute("aria-labelledby", titleId);
    surface.setAttribute("aria-describedby", descriptionId);
    const titleWrap = makeElement("div", "heading-dialog min-w-0 font-semibold");
    const title = makeElement("h2", "", typeof settings.title === "string" ? settings.title : "Are you sure?");
    title.id = titleId;
    titleWrap.append(title);
    const descriptionWrap = makeElement("div", "text-codex-description text-base leading-normal tracking-normal");
    const description = makeElement("p", "", typeof settings.description === "string" ? settings.description : "");
    description.id = descriptionId;
    descriptionWrap.append(description);
    const copy = makeElement("div", "flex min-w-0 flex-1 flex-col gap-1 self-stretch");
    copy.append(titleWrap, descriptionWrap);
    const headerRow = makeElement("div", "flex flex-col items-start gap-3");
    headerRow.append(copy);
    const headerSection = makeElement("div", "flex w-full flex-col first:pt-0 pt-3");
    headerSection.append(headerRow);
    const cancel = makeElement("button", `${dialogButtonClasses} text-default bg-text/5 not-disabled:not-aria-disabled:hover:bg-text/10 data-[state=open]:bg-text/10`, typeof settings.cancelLabel === "string" ? settings.cancelLabel : "Cancel");
    cancel.type = "button";
    cancel.dataset.swapdexConfirmCancel = "true";
    const confirm = makeElement("button", `${dialogButtonClasses} bg-chart-red/10 not-disabled:not-aria-disabled:hover:bg-chart-red/20 text-chart-red`, typeof settings.confirmLabel === "string" ? settings.confirmLabel : "Confirm");
    confirm.type = "submit";
    confirm.dataset.swapdexConfirmAccept = "true";
    const actionRow = makeElement("div", "flex w-full items-center justify-end gap-3");
    actionRow.append(cancel, confirm);
    const actionSection = makeElement("div", "flex w-full flex-col first:pt-0 pt-3");
    actionSection.append(actionRow);
    const form = makeElement("form", "flex flex-col gap-0 text-base leading-normal tracking-normal px-5 py-5");
    form.append(headerSection, actionSection);
    const commit = typeof settings.onConfirm === "function" ? settings.onConfirm : null;
    form.addEventListener("submit", event => {
      event.preventDefault();
      event.stopPropagation();
      destroyConfirmation();
      if (commit) {
        commit();
      }
    });
    cancel.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      destroyConfirmation();
    });
    overlay.addEventListener("pointerdown", event => {
      if (event.target === overlay) {
        event.preventDefault();
        destroyConfirmation();
      }
    });
    const onKeyDown = event => {
      if (event.key !== "Tab") {
        return;
      }
      if (event.shiftKey && document.activeElement === cancel) {
        event.preventDefault();
        confirm.focus({ preventScroll: true });
      } else if (!event.shiftKey && document.activeElement === confirm) {
        event.preventDefault();
        cancel.focus({ preventScroll: true });
      }
    };
    surface.append(form);
    document.body.append(overlay, surface);
    confirmationDialog = { overlay, surface, onKeyDown, restoreFocus };
    window.addEventListener("keydown", onKeyDown, true);
    cancel.focus({ preventScroll: true });
  };

  const dismissConfirmationOnEscape = event => {
    if (!confirmationDialog || event.key !== "Escape" || event.defaultPrevented) {
      return;
    }
    event.preventDefault();
    event.stopImmediatePropagation();
    event.stopPropagation();
    destroyConfirmation();
  };

  window.addEventListener("keydown", dismissConfirmationOnEscape, true);
  document.addEventListener("keydown", dismissConfirmationOnEscape, true);
  const pendingReauths = new Set();

  const normalize = value => String(value ?? "").replace(/\s+/g, " ").trim();

  const validAvatarProfile = id => typeof id === "string" && /^[A-Za-z0-9_-]{1,64}$/.test(id);
  const validAvatarSource = source => {
    if (typeof source !== "string" || source.length === 0) {
      return false;
    }
    if (source.length <= 2048 && /^https:\/\//i.test(source)) {
      return true;
    }
    return source.length <= 262144 && /^data:image\/(?:jpeg|png|webp|gif);base64,[A-Za-z0-9+/]+={0,2}$/i.test(source);
  };
  const ownerAvatarSource = owner => {
    const image = owner?.querySelector("img");
    const source = image?.currentSrc || image?.getAttribute("src") || "";
    return validAvatarSource(source) ? source : "";
  };
  const storedAvatar = id => {
    if (!validAvatarProfile(id)) {
      return "";
    }
    try {
      const source = localStorage.getItem(avatarStoragePrefix + id) || "";
      return validAvatarSource(source) ? source : "";
    } catch {
      return "";
    }
  };
  const rememberAvatar = (id, owner) => {
    const source = ownerAvatarSource(owner);
    if (!validAvatarProfile(id) || source.length === 0) {
      return;
    }
    try {
      const key = avatarStoragePrefix + id;
      if (localStorage.getItem(key) !== source) {
        localStorage.setItem(key, source);
      }
    } catch {
      return;
    }
  };
  const avatarWrapper = (row, image) => {
    let wrapper = image;
    while (wrapper?.parentElement && wrapper.parentElement !== row && wrapper.parentElement.children.length === 1) {
      wrapper = wrapper.parentElement;
    }
    return wrapper;
  };
  const fallbackAvatarSources = new Map();
  const fallbackAvatarSource = (profile, colorSource) => {
    // Rows without a remembered picture used to drop the avatar entirely. Render a
    // monogram circle in the same muted style as the settings account fallback so
    // every account still shows a picture.
    const initial = accountAvatarInitial(profile);
    const colorMatch = /rgba?\(\s*(\d+)[\s,]+(\d+)[\s,]+(\d+)/.exec(getComputedStyle(colorSource instanceof Element ? colorSource : document.body).color);
    const [r, g, b] = colorMatch ? [Number(colorMatch[1]), Number(colorMatch[2]), Number(colorMatch[3])] : [128, 128, 128];
    const key = `${initial} ${r} ${g} ${b}`;
    if (fallbackAvatarSources.has(key)) {
      return fallbackAvatarSources.get(key);
    }
    let source = "";
    const canvas = document.createElement("canvas");
    canvas.width = 96;
    canvas.height = 96;
    const context = canvas.getContext("2d");
    if (context) {
      context.beginPath();
      context.arc(48, 48, 47, 0, Math.PI * 2);
      context.fillStyle = `rgba(${r}, ${g}, ${b}, 0.1)`;
      context.fill();
      context.lineWidth = 2;
      context.strokeStyle = `rgba(${r}, ${g}, ${b}, 0.18)`;
      context.stroke();
      context.fillStyle = `rgba(${r}, ${g}, ${b}, 0.72)`;
      context.font = "600 42px -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif";
      context.textAlign = "center";
      context.textBaseline = "middle";
      context.fillText(initial, 48, 50);
      const dataUrl = canvas.toDataURL("image/png");
      if (validAvatarSource(dataUrl)) {
        source = dataUrl;
      }
    }
    fallbackAvatarSources.set(key, source);
    return source;
  };
  const applyAvatar = (row, profile, owner) => {
    const image = row.querySelector("img");
    if (!image) {
      return;
    }
    const wrapper = avatarWrapper(row, image);
    const source = storedAvatar(profile?.id) || fallbackAvatarSource(profile, owner);
    if (source.length === 0) {
      if (wrapper instanceof HTMLElement) {
        wrapper.hidden = true;
        wrapper.setAttribute("aria-hidden", "true");
      }
      return;
    }
    if (wrapper instanceof HTMLElement) {
      wrapper.hidden = false;
      wrapper.removeAttribute("aria-hidden");
    }
    image.removeAttribute("srcset");
    image.alt = "";
    image.src = source;
  };

  const accountAvatarInitial = profile => {
    const value = normalize(profile?.email || profile?.label || "A");
    return (Array.from(value)[0] || "A").toUpperCase();
  };
  const applySettingsAvatar = (container, profile) => {
    if (!(container instanceof HTMLElement) || !profile || typeof profile.id !== "string") {
      return;
    }
    const image = container.querySelector("[data-swapdex-account-avatar-image]");
    const fallback = container.querySelector("[data-swapdex-account-avatar-fallback]");
    if (!(image instanceof HTMLImageElement) || !(fallback instanceof HTMLElement)) {
      return;
    }
    setElementText(fallback, accountAvatarInitial(profile));
    const source = storedAvatar(profile.id);
    const showFallback = () => {
      image.hidden = true;
      image.removeAttribute("src");
      image.removeAttribute("srcset");
      fallback.hidden = false;
    };
    if (!source) {
      image.onload = null;
      image.onerror = null;
      showFallback();
      return;
    }
    const showImage = () => {
      if (image.src !== source) {
        return;
      }
      image.hidden = false;
      fallback.hidden = true;
    };
    image.onload = showImage;
    image.onerror = () => {
      if (image.src === source) {
        showFallback();
      }
    };
    image.hidden = true;
    fallback.hidden = false;
    image.src = source;
    if (image.complete && image.naturalWidth > 0) {
      showImage();
    }
  };

  const validMaintenanceIntervals = [5, 12, 24];
  const normalizeHiddenMenuRows = value => {
    if (!Array.isArray(value)) {
      return [];
    }
    return [...new Set(value.filter(id => typeof id === "string" && optionalMenuIds.has(id)))].sort();
  };
  const readPreferences = () => {
    const result = { blurProfileNames: false, maintenanceEnabled: true, maintenanceIntervalHours: 12, hiddenNativeProfileMenuRows: [] };
    let stored = {};
    try {
      const raw = localStorage.getItem(preferenceStorageKey);
      if (raw) {
        const parsed = JSON.parse(raw);
        if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
          stored = parsed;
        }
      }
      if (typeof stored.blurProfileNames !== "boolean") {
        stored.blurProfileNames = localStorage.getItem(privacyStorageKey) === "true";
      }
      if (typeof stored.maintenanceEnabled !== "boolean") {
        const legacyEnabled = localStorage.getItem(maintenanceEnabledStorageKey);
        stored.maintenanceEnabled = legacyEnabled === null ? true : legacyEnabled === "true";
      }
      if (!validMaintenanceIntervals.includes(stored.maintenanceIntervalHours)) {
        const legacyInterval = Number(localStorage.getItem(maintenanceIntervalStorageKey));
        stored.maintenanceIntervalHours = validMaintenanceIntervals.includes(legacyInterval) ? legacyInterval : 12;
      }
    } catch {
      return result;
    }
    result.blurProfileNames = stored.blurProfileNames === true;
    result.maintenanceEnabled = stored.maintenanceEnabled === true;
    result.maintenanceIntervalHours = validMaintenanceIntervals.includes(stored.maintenanceIntervalHours) ? stored.maintenanceIntervalHours : 12;
    result.hiddenNativeProfileMenuRows = normalizeHiddenMenuRows(stored.hiddenNativeProfileMenuRows);
    return result;
  };

  const writePreferences = () => {
    let saved = true;
    try {
      localStorage.setItem(preferenceStorageKey, JSON.stringify({ blurProfileNames, maintenanceEnabled, maintenanceIntervalHours, hiddenNativeProfileMenuRows: [...hiddenNativeProfileMenuRows].sort() }));
    } catch {
      saved = false;
    }
    try {
      localStorage.setItem(privacyStorageKey, blurProfileNames ? "true" : "false");
    } catch {
      saved = false;
    }
    try {
      localStorage.setItem(maintenanceEnabledStorageKey, maintenanceEnabled ? "true" : "false");
    } catch {
      saved = false;
    }
    try {
      localStorage.setItem(maintenanceIntervalStorageKey, String(maintenanceIntervalHours));
    } catch {
      saved = false;
    }
    preferenceSaveState = saved ? "saved" : "failed";
  };

  const queueMaintenanceConfig = () => {
    enqueue({
      v: 1,
      action: "maintenance-config",
      key: null,
      source: "profile-dropdown",
      enabled: maintenanceEnabled,
      interval_hours: maintenanceIntervalHours
    });
  };

  const applyPrivacyPreference = () => {
    if (blurProfileNames) {
      document.documentElement.setAttribute("data-swapdex-blur-profile-names", "true");
    } else {
      document.documentElement.removeAttribute("data-swapdex-blur-profile-names");
    }
  };

  const markProfileName = element => {
    if (element instanceof HTMLElement) {
      element.setAttribute("data-swapdex-profile-name", "true");
    }
  };

  const markProfileTriggerName = () => {
    const triggers = Array.from(document.querySelectorAll('button[aria-haspopup="menu"]')).filter(button => triggerLabels.has(normalize(button.getAttribute("aria-label"))));
    for (const trigger of triggers) {
      const nodes = nonemptyTextNodes(trigger);
      if (nodes.length >= 1) {
        markProfileName(nodes[0].parentElement);
      }
    }
  };

  const storedPreferences = readPreferences();
  blurProfileNames = storedPreferences.blurProfileNames;
  maintenanceEnabled = storedPreferences.maintenanceEnabled;
  maintenanceIntervalHours = storedPreferences.maintenanceIntervalHours;
  hiddenNativeProfileMenuRows = new Set(storedPreferences.hiddenNativeProfileMenuRows);
  applyPrivacyPreference();

  Object.defineProperty(window, "__swapdexTakePendingRequest", {
    value: () => pendingRequests.shift() ?? null,
    configurable: false,
    enumerable: false,
    writable: false
  });

  // Anything that goes wrong in here used to be invisible, because a throw during
  // evaluation only fails the injection and says nothing about why. Failures are
  // reported to the service so they land in the service log.
  const reportRendererFailure = (scope, error) => {
    try {
      const text = String((error && error.message) || error);
      if (pendingRequests.length >= 8) {
        pendingRequests.shift();
      }
      pendingRequests.push(JSON.stringify({ v: 1, source: "profile-dropdown", action: "renderer-error", scope: String(scope).slice(0, 40), text: text.slice(0, 300) }));
    } catch {
    }
  };

  const enqueue = payload => {
    if (pendingRequests.length >= 8) {
      pendingRequests.shift();
    }
    pendingRequests.push(JSON.stringify(payload));
  };

  queueMaintenanceConfig();

  const request = action => {
    enqueue({
      v: 1,
      action,
      key: null,
      source: "profile-dropdown"
    });
  };

  const ownedRows = menu => Array.from(menu.querySelectorAll('[role="menuitem"]')).filter(row => {
    if (row.closest('[role="menu"]') !== menu || row.hidden || row.getAttribute("aria-disabled") === "true") {
      return false;
    }
    const style = getComputedStyle(row);
    return style.display !== "none" && style.visibility !== "hidden";
  });

  const hasExactLabel = (row, expected) => Array.from(row.querySelectorAll("span, div")).some(element => {
    if (element.children.length !== 0 || element.querySelector("svg, img") !== null) {
      return false;
    }
    return normalize(element.textContent) === expected;
  });

  const nonemptyTextNodes = row => {
    const walker = document.createTreeWalker(row, NodeFilter.SHOW_TEXT);
    const nodes = [];
    let node = walker.nextNode();
    while (node) {
      if (normalize(node.textContent) && !node.parentElement?.closest("svg, style, script")) {
        nodes.push(node);
      }
      node = walker.nextNode();
    }
    return nodes;
  };

  const elementStructure = row => JSON.stringify([row, ...row.querySelectorAll("*")].map(element => [
    element.tagName,
    typeof element.className === "string" ? element.className : ""
  ]));

  const isNativeOwnerRow = row => {
    const nodes = nonemptyTextNodes(row);
    const firstContainer = nodes[0]?.parentElement;
    const secondContainer = nodes[1]?.parentElement;
    const content = firstContainer?.parentElement;
    return nodes.length === 2 && firstContainer !== secondContainer && content?.contains(secondContainer) && getComputedStyle(content).flexDirection === "column";
  };

  const nativeMenuRows = menu => Array.from(menu.querySelectorAll('[role="menuitem"]')).filter(row => row.closest('[role="menu"]') === menu && !row.hasAttribute("data-swapdex-row"));

  const optionalMenuRow = (rows, item) => {
    const matches = rows.filter(row => item.labels.some(label => hasExactLabel(row, label)));
    return matches.length === 1 ? matches[0] : null;
  };
  const applyNativeMenuVisibility = identified => {
    const rows = identified.nativeRows || nativeMenuRows(identified.menu);
    for (const item of optionalMenuItems) {
      const row = optionalMenuRow(rows, item);
      if (!(row instanceof HTMLElement)) {
        continue;
      }
      if (!nativeMenuRowState.has(row)) {
        nativeMenuRowState.set(row, { hidden: row.hidden, ariaHidden: row.getAttribute("aria-hidden"), tabIndex: row.getAttribute("tabindex") });
      }
      const original = nativeMenuRowState.get(row);
      row.dataset.swapdexMenuRow = item.id;
      if (hiddenNativeProfileMenuRows.has(item.id)) {
        row.hidden = true;
        row.setAttribute("aria-hidden", "true");
        row.setAttribute("tabindex", "-1");
        row.dataset.swapdexHiddenNativeRow = item.id;
      } else {
        row.hidden = original.hidden;
        if (original.ariaHidden === null) {
          row.removeAttribute("aria-hidden");
        } else {
          row.setAttribute("aria-hidden", original.ariaHidden);
        }
        if (original.tabIndex === null) {
          row.removeAttribute("tabindex");
        } else {
          row.setAttribute("tabindex", original.tabIndex);
        }
        delete row.dataset.swapdexHiddenNativeRow;
      }
    }
  };

  const identifyProfileMenu = () => {
    if (!supportedLanguage) {
      return null;
    }
    const candidates = Array.from(document.querySelectorAll('[role="menu"][data-radix-menu-content][data-state="open"]')).flatMap(menu => {
      const labelledBy = menu.getAttribute("aria-labelledby");
      const triggerIds = labelledBy ? labelledBy.split(/\s+/).filter(Boolean) : [];
      const triggers = triggerIds.map(id => document.getElementById(id)).filter(Boolean);
      const trigger = triggers.length === 1 ? triggers[0] : null;
      if (!menu.id || !trigger || !triggerLabels.has(normalize(trigger.getAttribute("aria-label")))) {
        return [];
      }
      if (trigger.getAttribute("aria-haspopup") !== "menu" || trigger.getAttribute("aria-expanded") !== "true" || trigger.getAttribute("aria-controls") !== menu.id) {
        return [];
      }
      const rows = nativeMenuRows(menu);
      const settings = rows.filter(row => hasExactLabel(row, settingsLabel));
      const logOut = rows.filter(row => hasExactLabel(row, logOutLabel));
      if (settings.length !== 1 || logOut.length > 1) {
        return [];
      }
      const shell = settings[0].parentElement;
      const owner = rows.find(row => isNativeOwnerRow(row));
      if (!owner || owner === settings[0] || owner === logOut[0] || owner.parentElement !== shell || shell?.parentElement !== menu || (logOut[0] && rows.at(-1) !== logOut[0])) {
        return [];
      }
      return [{ menu, trigger, shell, owner, settings: settings[0], logOut: logOut[0] || null, nativeRows: rows }];
    });
    return candidates.length === 1 ? candidates[0] : null;
  };

  const sanitizeClone = row => {
    const clone = row.cloneNode(true);
    clone.removeAttribute("id");
    clone.removeAttribute("aria-controls");
    clone.removeAttribute("aria-describedby");
    clone.removeAttribute("aria-expanded");
    clone.removeAttribute("aria-haspopup");
    clone.removeAttribute("aria-current");
    clone.removeAttribute("aria-selected");
    clone.removeAttribute("aria-disabled");
    clone.removeAttribute("disabled");
    clone.removeAttribute("data-disabled");
    clone.removeAttribute("data-state");
    clone.removeAttribute("data-highlighted");
    clone.removeAttribute("data-disabled");
    clone.setAttribute("role", "menuitem");
    clone.setAttribute("tabindex", "-1");
    for (const element of [clone, ...clone.querySelectorAll("*")]) {
      for (const attribute of Array.from(element.attributes)) {
        if (attribute.name.startsWith("on") || attribute.name === "id" || attribute.name === "aria-controls" || attribute.name === "aria-describedby" || attribute.name === "aria-labelledby") {
          element.removeAttribute(attribute.name);
        }
      }
      element.removeAttribute("data-state");
      element.removeAttribute("data-highlighted");
      element.removeAttribute("aria-disabled");
      element.removeAttribute("disabled");
      element.removeAttribute("data-disabled");
      if ("disabled" in element) {
        element.disabled = false;
      }
    }
    return clone;
  };

  const setPersonIcon = row => {
    const icon = row.querySelector("svg");
    if (!(icon instanceof SVGElement)) {
      return false;
    }
    const nativeProfileIcon = Array.from(document.querySelectorAll("svg")).find(candidate => {
      const markup = candidate.outerHTML;
      return markup.includes("M8 4.1416") && markup.includes("M8 1.47461");
    });
    if (nativeProfileIcon instanceof SVGElement) {
      const replacement = nativeProfileIcon.cloneNode(true);
      replacement.removeAttribute("id");
      replacement.setAttribute("data-swapdex-icon", "profile");
      replacement.setAttribute("aria-hidden", "true");
      replacement.removeAttribute("aria-label");
      replacement.removeAttribute("title");
      icon.replaceWith(replacement);
      return true;
    }
    icon.setAttribute("data-swapdex-icon", "profile");
    icon.setAttribute("aria-hidden", "true");
    icon.removeAttribute("aria-label");
    icon.removeAttribute("title");
    icon.innerHTML = '<path fill-rule="evenodd" clip-rule="evenodd" d="M8 4.1416C9.39452 4.1416 10.5254 5.27247 10.5254 6.66699C10.5251 8.06129 9.39436 9.19238 8 9.19238C6.60564 9.19238 5.47487 8.06129 5.47461 6.66699C5.47461 5.27247 6.60548 4.1416 8 4.1416ZM8 5.19238C7.18538 5.19238 6.52539 5.85237 6.52539 6.66699C6.52565 7.48139 7.18554 8.1416 8 8.1416C8.81446 8.1416 9.47435 7.48139 9.47461 6.66699C9.47461 5.85237 8.81462 5.19238 8 5.19238Z" fill="currentColor"></path><path fill-rule="evenodd" clip-rule="evenodd" d="M8 1.47461C11.6037 1.47461 14.5254 4.39634 14.5254 8C14.5254 11.6037 11.6037 14.5254 8 14.5254C4.39634 14.5254 1.47461 11.6037 1.47461 8C1.47461 4.39634 4.39634 1.47461 8 1.47461ZM8 11.1924C6.82222 11.1925 5.78204 11.7791 5.15332 12.6768C5.98281 13.1827 6.95721 13.4746 8 13.4746C9.04297 13.4746 10.0171 13.1819 10.8467 12.6758C10.2178 11.7786 9.17734 11.1925 8 11.1924ZM8 2.52539C4.97624 2.52539 2.52539 4.97624 2.52539 8C2.52539 9.60307 3.21462 11.0447 4.3125 12.0459C5.13249 10.8944 6.47756 10.1417 8 10.1416C9.52227 10.1417 10.8667 10.8946 11.6865 12.0459C12.7847 11.0447 13.4746 9.60333 13.4746 8C13.4746 4.97624 11.0238 2.52539 8 2.52539Z" fill="currentColor"></path>';
    return true;
  };

  const setRowText = (row, value) => {
    const walker = document.createTreeWalker(row, NodeFilter.SHOW_TEXT);
    let assigned = false;
    let node = walker.nextNode();
    while (node) {
      const parent = node.parentElement;
      if (parent && !parent.closest("svg, style, script") && normalize(node.textContent)) {
        node.textContent = assigned ? "" : value;
        assigned = true;
      }
      node = walker.nextNode();
    }
    if (!assigned) {
      const label = document.createElement("span");
      label.textContent = value;
      row.append(label);
    }
    row.setAttribute("aria-label", value);
  };

  const setOwnerRowText = (row, identity, detail) => {
    const nodes = nonemptyTextNodes(row);
    if (nodes.length !== 2) {
      return false;
    }
    nodes[0].textContent = identity;
    nodes[1].textContent = detail;
    markProfileName(nodes[0].parentElement);
    row.setAttribute("aria-label", `${identity}, ${detail}`);
    row.setAttribute("title", `${identity} — ${detail}`);
    return true;
  };

  const remaining = value => Number.isFinite(value) ? `${Math.max(0, Math.min(100, Math.round(value)))}%` : "—";
  const usageParts = profile => {
    const parts = [];
    if (Number.isFinite(profile.primary_remaining)) {
      parts.push(`5h ${remaining(profile.primary_remaining)}`);
    }
    if (Number.isFinite(profile.secondary_remaining)) {
      parts.push(`7d ${remaining(profile.secondary_remaining)}`);
    }
    return parts;
  };

  const compactNumber = value => {
    const absolute = Math.abs(value);
    if (absolute >= 1000000000) {
      return `${(value / 1000000000).toFixed(1).replace(/\.0$/, "")}B`;
    }
    if (absolute >= 1000000) {
      return `${(value / 1000000).toFixed(1).replace(/\.0$/, "")}M`;
    }
    if (absolute >= 1000) {
      return `${(value / 1000).toFixed(1).replace(/\.0$/, "")}k`;
    }
    return String(Math.round(value));
  };
  const accountCreditsText = profile => {
    if (profile.credits_unlimited === true) {
      return "Unlimited credits";
    }
    const balance = normalize(profile.credits_balance);
    if (!balance) {
      return "";
    }
    const numeric = Number(balance);
    if (Number.isFinite(numeric) && numeric <= 0) {
      return "0 credits";
    }
    const formatted = Number.isFinite(numeric) && Math.abs(numeric) >= 1000 ? compactNumber(numeric) : balance;
    return `${formatted} credits`;
  };
  const accountResetsText = profile => {
    const resets = Number(profile.available_reset_credits ?? profile.available_credits);
    if (!Number.isFinite(resets) || resets <= 0) {
      return "";
    }
    return `${compactNumber(resets)} reset${resets === 1 ? "" : "s"}`;
  };

  const activate = (menu, row, action, key) => {
    if (!menu.isConnected || !row.isConnected || activatedRows.has(row)) {
      return;
    }
    activatedRows.add(row);
    setTimeout(() => activatedRows.delete(row), 1000);
    enqueue({
      v: 1,
      action,
      key: key ?? null,
      source: "profile-dropdown"
    });
    menu.dispatchEvent(new KeyboardEvent("keydown", {
      key: "Escape",
      code: "Escape",
      bubbles: true,
      cancelable: true
    }));
  };

  document.addEventListener("click", event => {
    const row = event.target instanceof Element ? event.target.closest('[data-swapdex-row]') : null;
    const menu = row?.closest('[role="menu"][data-state="open"]');
    if (!row || !menu || !menu.contains(row)) {
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    activate(menu, row, row.dataset.swapdexAction, row.dataset.swapdexKey || null);
  }, true);

  const enhanceMenu = identified => {
    const { menu, owner, shell, logOut, settings } = identified;
    installSettingsStyles();
    if (!menuState.has(menu)) {
      menuState.set(menu, { generation: -1, signature: "" });
      menu.addEventListener("pointermove", event => {
        const row = event.target instanceof Element ? event.target.closest('[role="menuitem"]') : null;
        if (row && ownedRows(menu).includes(row)) {
          row.focus({ preventScroll: true });
        }
      });
      menu.addEventListener("focusin", event => {
        const row = event.target instanceof Element ? event.target.closest('[role="menuitem"]') : null;
        if (row && ownedRows(menu).includes(row)) {
          row.setAttribute("data-highlighted", "");
        } else if (row?.hasAttribute("data-swapdex-hidden-native-row")) {
          const visible = ownedRows(menu);
          const allRows = nativeMenuRows(menu);
          const index = allRows.indexOf(row);
          const next = visible.find(candidate => allRows.indexOf(candidate) > index) || visible.at(-1);
          next?.focus({ preventScroll: true });
        }
      });
      menu.addEventListener("keydown", event => {
        const row = event.target instanceof Element ? event.target.closest('[role="menuitem"]') : null;
        if (row?.hasAttribute("data-swapdex-hidden-native-row")) {
          event.preventDefault();
          event.stopPropagation();
          ownedRows(menu)[0]?.focus({ preventScroll: true });
          return;
        }
        if (!row || !ownedRows(menu).includes(row)) {
          return;
        }
        const currentRows = ownedRows(menu);
        const index = currentRows.indexOf(row);
        let next = null;
        if (event.key === "ArrowDown") {
          next = currentRows[(index + 1) % currentRows.length];
        } else if (event.key === "ArrowUp") {
          next = currentRows[(index - 1 + currentRows.length) % currentRows.length];
        } else if (event.key === "Home") {
          next = currentRows[0];
        } else if (event.key === "End") {
          next = currentRows.at(-1);
        }
        if (next) {
          event.preventDefault();
          event.stopPropagation();
          next.focus({ preventScroll: true });
          return;
        }
        if (row.hasAttribute("data-swapdex-row") && (event.key === "Enter" || event.key === " ")) {
          event.preventDefault();
          event.stopPropagation();
          activate(menu, row, row.dataset.swapdexAction, row.dataset.swapdexKey || null);
        }
      });
    }
    const state = menuState.get(menu);
    markProfileName(nonemptyTextNodes(owner)[0]?.parentElement);
    rememberAvatar(snapshot.active, owner);
    const activeProfile = Array.isArray(snapshot.profiles) ? snapshot.profiles.find(profile => profile && profile.id === snapshot.active) : null;
    if (activeProfile) {
      setOwnerRowText(owner, normalize(activeProfile.email || activeProfile.label || "Account"), profileUsageDetail(activeProfile));
    }
    applyProfileMenuUsage(owner, activeProfile);
    applyNativeMenuVisibility(identified);
    const others = Array.isArray(snapshot.profiles)
      ? snapshot.profiles.filter(profile => profile.id !== snapshot.active && profile.authenticated !== false)
      : [];
    const signature = JSON.stringify(others.map(profile => [profile.id, profile.email || profile.label || "Account", profile.plan, ...usageParts(profile), storedAvatar(profile.id)]));
    const accountRows = Array.from(menu.querySelectorAll('[data-swapdex-row="account"]'));
    const addRows = Array.from(menu.querySelectorAll('[data-swapdex-row="add"]'));
    const ownerStructure = elementStructure(owner);
    const exactRows = accountRows.length === others.length && addRows.length === 1 && accountRows.every((row, index) => row.dataset.swapdexKey === String(others[index].id) && elementStructure(row) === ownerStructure);
    if (state.generation === renderGeneration && state.signature === signature && state.owner === owner && exactRows) {
      renderStatus(identified);
      return;
    }
    const focusedKey = document.activeElement instanceof HTMLElement ? document.activeElement.dataset.swapdexKey || null : null;
    for (const existing of Array.from(menu.querySelectorAll('[data-swapdex-row]'))) {
      existing.remove();
    }
    const accountTemplate = sanitizeClone(owner);
    const fragment = document.createDocumentFragment();
    for (const profile of others) {
      const row = sanitizeClone(accountTemplate);
      row.setAttribute("data-swapdex-row", "account");
      row.setAttribute("data-swapdex-action", "switch");
      row.setAttribute("data-swapdex-key", String(profile.id));
      const identity = normalize(profile.email || profile.label || "Account");
      const detail = profileUsageDetail(profile);
      if (!setOwnerRowText(row, identity, detail)) {
        return;
      }
      applyProfileMenuUsage(row, profile);
      applyAvatar(row, profile, owner);
      fragment.append(row);
    }
    shell.insertBefore(fragment, owner);
    const addRow = sanitizeClone(settings);
    addRow.setAttribute("data-swapdex-row", "add");
    addRow.setAttribute("data-swapdex-action", "add");
    setRowText(addRow, "Add account");
    setPersonIcon(addRow);
    if (logOut) {
      shell.insertBefore(addRow, logOut);
    } else {
      shell.append(addRow);
    }
    state.generation = renderGeneration;
    state.signature = signature;
    state.owner = owner;
    if (focusedKey) {
      const replacement = menu.querySelector(`[data-swapdex-key="${CSS.escape(focusedKey)}"]`);
      if (replacement instanceof HTMLElement) {
        replacement.focus({ preventScroll: true });
      }
    }
    if (pendingStatus) {
      renderStatus(identified);
    }
  };

  const renderStatus = identified => {
    for (const existing of Array.from(identified.menu.querySelectorAll('[data-swapdex-status]'))) {
      existing.remove();
    }
    if (!pendingStatus) {
      return;
    }
    const row = sanitizeClone(identified.settings);
    row.setAttribute("data-swapdex-status", "true");
    row.setAttribute("role", "status");
    row.setAttribute("aria-live", "polite");
    row.setAttribute("tabindex", "-1");
    setRowText(row, pendingStatus);
    if (identified.logOut) {
      identified.shell.insertBefore(row, identified.logOut);
    } else {
      identified.shell.append(row);
    }
  };

  const visibleElement = element => {
    if (!(element instanceof HTMLElement) || !element.isConnected) {
      return false;
    }
    const style = getComputedStyle(element);
    const rect = element.getBoundingClientRect();
    return style.display !== "none" && style.visibility !== "hidden" && rect.width > 0 && rect.height > 0;
  };

  const makeElement = (tag, className = "", text = "") => {
    const element = document.createElement(tag);
    if (className.length > 0) {
      element.className = className;
    }
    if (text.length > 0) {
      element.textContent = text;
    }
    return element;
  };

  const setElementText = (element, value) => {
    if (element.textContent !== value) {
      element.textContent = value;
    }
  };

  const usagePercent = value => {
    if (value === null || value === undefined || value === "") {
      return null;
    }
    const numeric = Number(value);
    return Number.isFinite(numeric) ? Math.max(0, Math.min(100, Math.round(numeric))) : null;
  };
  const createUsageMeter = (key, label) => {
    const meter = makeElement("span", "swapdex-usage-meter");
    meter.dataset.swapdexUsageMeter = key;
    meter.setAttribute("role", "group");
    const meterLabel = makeElement("span", "swapdex-usage-label", label);
    const track = makeElement("span", "swapdex-usage-track");
    track.setAttribute("aria-hidden", "true");
    const fill = makeElement("span", "swapdex-usage-fill");
    fill.dataset.swapdexUsageFill = "true";
    const value = makeElement("span", "swapdex-usage-value");
    value.dataset.swapdexUsageValue = "true";
    track.append(fill);
    meter.append(meterLabel, track, value);
    return meter;
  };
  const updateUsageMeter = (meter, value, label) => {
    const percent = usagePercent(value);
    const fill = meter.querySelector("[data-swapdex-usage-fill]");
    const text = meter.querySelector("[data-swapdex-usage-value]");
    meter.hidden = percent === null;
    if (percent === null || !(fill instanceof HTMLElement) || !(text instanceof HTMLElement)) {
      return false;
    }
    fill.style.width = `${percent}%`;
    setElementText(text, `${percent}%`);
    meter.setAttribute("aria-label", `${label} ${percent}% remaining`);
    return true;
  };
  const capitalizedPlan = profile => {
    const plan = normalize(profile?.plan);
    if (!plan || plan.toLowerCase() === "unknown") {
      return "";
    }
    return plan.charAt(0).toUpperCase() + plan.slice(1);
  };
  const bankedResetsText = profile => {
    const resets = Number(profile?.available_reset_credits ?? profile?.available_credits);
    if (!Number.isFinite(resets) || resets <= 0) {
      return "";
    }
    return `${compactNumber(resets)} reset${resets === 1 ? "" : "s"} available`;
  };
  const profileUsageDetail = profile => [capitalizedPlan(profile), bankedResetsText(profile)].filter(Boolean).join(" · ") || "Account";
  const resetAdviceText = profile => {
    if (bankedResetsText(profile).length === 0) {
      return "";
    }
    const now = Math.floor(Date.now() / 1000);
    const expiries = Array.isArray(profile?.reset_credits)
      ? profile.reset_credits.map(item => Number(item?.expires_at)).filter(value => Number.isFinite(value) && value > now)
      : [];
    if (expiries.length > 0) {
      const date = new Date(Math.min(...expiries) * 1000);
      return `Use by ${date.toLocaleDateString(undefined, { month: "short", day: "numeric", year: "numeric" })}`;
    }
    return "Use before your next limit reset";
  };
  const createProfileMenuUsage = () => {
    const usage = makeElement("div", "swapdex-profile-usage");
    usage.dataset.swapdexProfileUsage = "true";
    usage.setAttribute("aria-hidden", "true");
    for (const [key, label] of [["primary", "5h"], ["secondary", "7d"]]) {
      const meter = makeElement("span", "swapdex-profile-usage-meter");
      meter.dataset.swapdexProfileUsageMeter = key;
      meter.dataset.swapdexProfileUsageLabel = label;
      const track = makeElement("span", "swapdex-profile-usage-track");
      const fill = makeElement("span", "swapdex-profile-usage-fill");
      fill.dataset.swapdexProfileUsageFill = "true";
      track.append(fill);
      meter.append(track);
      usage.append(meter);
    }
    const advice = makeElement("div", "swapdex-profile-reset-advice");
    advice.dataset.swapdexProfileResetAdvice = "true";
    advice.setAttribute("aria-hidden", "true");
    usage.append(advice);
    return usage;
  };
  const applyProfileMenuUsage = (row, profile) => {
    if (!(row instanceof HTMLElement)) {
      return;
    }
    const content = nonemptyTextNodes(row)[0]?.parentElement?.parentElement;
    if (!(content instanceof HTMLElement)) {
      return;
    }
    let usage = row.querySelector("[data-swapdex-profile-usage]");
    if (!(usage instanceof HTMLElement)) {
      usage = createProfileMenuUsage();
      content.append(usage);
    }
    const advice = usage.querySelector("[data-swapdex-profile-reset-advice]");
    const adviceText = resetAdviceText(profile);
    if (advice instanceof HTMLElement) {
      advice.dataset.swapdexProfileResetAdviceText = adviceText;
      advice.hidden = adviceText.length === 0;
    }
    let visible = false;
    for (const [key, property] of [["primary", "primary_remaining"], ["secondary", "secondary_remaining"]]) {
      const meter = usage.querySelector(`[data-swapdex-profile-usage-meter="${key}"]`);
      const fill = meter?.querySelector("[data-swapdex-profile-usage-fill]");
      const percent = usagePercent(profile?.[property]);
      if (!(meter instanceof HTMLElement) || !(fill instanceof HTMLElement)) {
        continue;
      }
      meter.hidden = percent === null;
      if (percent !== null) {
        fill.style.width = `${percent}%`;
        meter.dataset.swapdexProfileUsageValue = `${percent}%`;
        visible = true;
      } else {
        delete meter.dataset.swapdexProfileUsageValue;
      }
    }
    usage.hidden = !visible && adviceText.length === 0;
  };

  const setNativeButtonText = (button, expected, value) => {
    const textElement = Array.from(button.querySelectorAll("span, div")).find(element => element.children.length === 0 && normalize(element.textContent) === expected);
    if (textElement) {
      setElementText(textElement, value);
      return;
    }
    const walker = document.createTreeWalker(button, NodeFilter.SHOW_TEXT);
    let node = walker.nextNode();
    while (node) {
      if (normalize(node.textContent) && !node.parentElement?.closest("svg, style, script")) {
        setElementText(node, value);
        return;
      }
      node = walker.nextNode();
    }
  };

  const sanitizeSettingsClone = element => {
    const clone = element.cloneNode(true);
    for (const current of [clone, ...clone.querySelectorAll("*")]) {
      for (const attribute of Array.from(current.attributes)) {
        if (attribute.name.startsWith("on") || attribute.name === "id" || attribute.name === "aria-controls" || attribute.name === "aria-labelledby" || attribute.name === "aria-current" || attribute.name === "data-highlighted") {
          current.removeAttribute(attribute.name);
        }
      }
      if ("disabled" in current) {
        current.disabled = false;
      }
      current.removeAttribute("aria-disabled");
    }
    return clone;
  };

  const installSettingsStyles = () => {
    if (document.querySelector("style[data-swapdex-settings-style]")) {
      return;
    }
    const style = document.createElement("style");
    style.dataset.swapdexSettingsStyle = "true";
    style.textContent = `
      .swapdex-settings-page { color: inherit; padding-bottom: 2rem; }
      .swapdex-settings-header { margin-bottom: 1.25rem; }
      .swapdex-settings-header p, .swapdex-settings-panel > p, .swapdex-settings-card p { color: color-mix(in srgb, currentColor 68%, transparent); }
      .swapdex-settings-tabs { display: inline-flex; width: max-content; gap: 0.125rem; margin-bottom: 1.5rem; padding: 0.1875rem; border: 0; border-radius: 0.5rem; background: color-mix(in srgb, currentColor 8%, transparent); }
      .swapdex-settings-tab { border: 0; border-radius: 0.375rem; color: color-mix(in srgb, currentColor 68%, transparent); cursor: pointer; padding: 0.375rem 0.75rem; margin: 0; }
      .swapdex-settings-tab:hover { color: inherit; background: color-mix(in srgb, currentColor 8%, transparent); }
      .swapdex-settings-tab[aria-selected="true"] { color: inherit; background: color-mix(in srgb, currentColor 14%, transparent); }
      .swapdex-settings-tab:focus-visible { outline: 2px solid color-mix(in srgb, currentColor 70%, transparent); outline-offset: 2px; }
      .swapdex-settings-panel { min-width: 0; padding-top: 0 !important; padding-bottom: 0 !important; }
      .swapdex-menu-settings-list { display: flex; flex-direction: column; }
      .swapdex-menu-settings-row { display: flex; align-items: center; justify-content: space-between; gap: 1rem; padding: 0.875rem 0; border-top: 1px solid color-mix(in srgb, currentColor 12%, transparent); }
      .swapdex-menu-settings-row:first-child { padding-top: 0; border-top: 0; }
      .swapdex-menu-settings-row:last-child { padding-bottom: 0; }
      .swapdex-menu-settings-copy { display: flex; min-width: 0; flex-direction: column; gap: 0.25rem; }
      .swapdex-menu-settings-note { margin: 0.875rem 0 0; color: color-mix(in srgb, currentColor 62%, transparent); font-size: 0.75rem; line-height: 1rem; }
      .swapdex-settings-section-title { margin: 0; }
      .swapdex-settings-card { border: 1px solid color-mix(in srgb, currentColor 20%, transparent) !important; background: color-mix(in srgb, currentColor 5%, transparent); padding: 1rem; }
      html[data-swapdex-settings-active="true"] button.sidebar-item:not([data-swapdex-settings-nav]) { background-color: transparent !important; box-shadow: none !important; }
      html[data-swapdex-settings-active="true"] button.sidebar-item:not([data-swapdex-settings-nav]):hover { background-color: color-mix(in srgb, currentColor 8%, transparent) !important; }
      .swapdex-switch { position: relative; width: 2rem; height: 1.25rem; padding: 0; border: 0; border-radius: 999px; background: color-mix(in srgb, currentColor 10%, transparent); cursor: pointer; }
      .swapdex-switch-thumb { position: absolute; top: 0.125rem; left: 0.125rem; width: 0.875rem; height: 0.875rem; border-radius: 999px; background: currentColor; transition: transform 120ms ease; }
      .swapdex-switch[aria-checked="true"] { background: var(--swapdex-accent, #3b82f6); }
      .swapdex-switch[aria-checked="true"] .swapdex-switch-thumb { transform: translateX(0.75rem); }
      .swapdex-switch:focus-visible { outline: 2px solid color-mix(in srgb, currentColor 70%, transparent); outline-offset: 2px; }
      .swapdex-interval-trigger { display: inline-flex; min-width: 7rem; align-items: center; justify-content: space-between; gap: 0.5rem; border: 1px solid color-mix(in srgb, currentColor 20%, transparent); border-radius: 0.5rem; padding: 0.375rem 0.75rem; color: inherit; background: color-mix(in srgb, currentColor 8%, transparent); cursor: pointer; text-align: left; font-size: 0.875rem; line-height: 1.125rem; }
      .swapdex-interval-trigger:hover:not(:disabled) { background: color-mix(in srgb, currentColor 14%, transparent); }
      .swapdex-interval-trigger:focus-visible { outline: 2px solid color-mix(in srgb, currentColor 70%, transparent); outline-offset: 2px; }
      .swapdex-interval-trigger:disabled { cursor: not-allowed; opacity: 0.5; }
      .swapdex-interval-chevron { width: 1rem; height: 1rem; flex: 0 0 auto; color: color-mix(in srgb, currentColor 70%, transparent); transition: transform 120ms ease; }
      .swapdex-interval-menu { position: fixed; z-index: 80; display: flex; min-width: 9rem; flex-direction: column; gap: 0.125rem; padding: 0.25rem; border: 1px solid color-mix(in srgb, currentColor 20%, transparent); border-radius: 0.5rem; background: color-mix(in srgb, Canvas 92%, CanvasText 8%); box-shadow: 0 0.5rem 1.5rem rgba(0, 0, 0, 0.24); transform: translateX(-50%); }
      .swapdex-interval-menu-item { display: flex; width: 100%; align-items: center; justify-content: space-between; gap: 1rem; border: 0; border-radius: 0.375rem; padding: 0.5rem 0.625rem; color: inherit; background: transparent; cursor: pointer; text-align: start; }
      .swapdex-interval-menu-item:hover, .swapdex-interval-menu-item:focus-visible { background: color-mix(in srgb, currentColor 10%, transparent); outline: none; }
      .swapdex-interval-menu-item[aria-checked="true"] { background: color-mix(in srgb, currentColor 14%, transparent); }
      .swapdex-interval-menu-check { color: color-mix(in srgb, currentColor 70%, transparent); }
      .swapdex-maintenance-button { border: 1px solid color-mix(in srgb, currentColor 20%, transparent); border-radius: 0.5rem; padding: 0.375rem 0.75rem; color: inherit; background: color-mix(in srgb, currentColor 8%, transparent); cursor: pointer; }
      .swapdex-maintenance-button:hover { background: color-mix(in srgb, currentColor 14%, transparent); }
      .swapdex-maintenance-button:focus-visible, .swapdex-interval-menu-item:focus-visible { outline: 2px solid color-mix(in srgb, currentColor 70%, transparent); outline-offset: 2px; }
      .swapdex-account-list { display: flex; flex-direction: column; }
      .swapdex-account-row { display: flex; align-items: center; justify-content: space-between; gap: 1rem; padding: 0.75rem 0; border-top: 1px solid color-mix(in srgb, currentColor 12%, transparent); }
      .swapdex-account-identity { display: flex; min-width: 0; flex: 1 1 auto; flex-direction: column; gap: 0.25rem; }
      .swapdex-account-meta { display: flex; min-width: 0; align-items: center; gap: 0.5rem; flex-wrap: wrap; }
      .swapdex-account-avatar { display: inline-flex; width: 1.5rem; height: 1.5rem; flex: 0 0 1.5rem; align-items: center; justify-content: center; overflow: hidden; border: 1px solid color-mix(in srgb, currentColor 18%, transparent); border-radius: 999px; background: color-mix(in srgb, currentColor 10%, transparent); color: color-mix(in srgb, currentColor 72%, transparent); }
      .swapdex-account-avatar-image { width: 100%; height: 100%; object-fit: cover; }
      .swapdex-account-avatar-fallback { font-size: 0.7rem; font-weight: 600; line-height: 1; }
      .swapdex-account-detail, .swapdex-account-last-warmed { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .swapdex-account-divider { width: 1px; height: 0.875rem; flex: 0 0 1px; background: color-mix(in srgb, currentColor 20%, transparent); }
      .swapdex-account-insights { display: flex; min-width: 0; align-items: center; flex-wrap: wrap; gap: 0.375rem; color: color-mix(in srgb, currentColor 58%, transparent); font-size: 0.6875rem; line-height: 1rem; }
      .swapdex-account-reset-advice { flex: 0 0 100%; margin-top: 0.125rem; color: color-mix(in srgb, currentColor 58%, transparent); }
      .swapdex-account-usage { display: inline-flex; min-width: 0; align-items: center; gap: 0.5rem; overflow: hidden; white-space: nowrap; }
      .swapdex-account-usage-label { color: color-mix(in srgb, currentColor 48%, transparent); }
      .swapdex-usage-meter { display: inline-flex; align-items: center; gap: 0.25rem; }
      .swapdex-usage-label { color: color-mix(in srgb, currentColor 68%, transparent); }
      .swapdex-usage-track { display: inline-block; width: 2rem; height: 0.25rem; overflow: hidden; border-radius: 999px; background: color-mix(in srgb, currentColor 14%, transparent); }
      .swapdex-usage-fill { display: block; width: 0; height: 100%; border-radius: inherit; background: var(--swapdex-accent, #3b82f6); transition: width 160ms ease; }
      .swapdex-usage-value { min-width: 2.25rem; color: color-mix(in srgb, currentColor 72%, transparent); font-variant-numeric: tabular-nums; }
      .swapdex-account-insights-divider { width: 1px; height: 0.75rem; flex: 0 0 1px; background: color-mix(in srgb, currentColor 18%, transparent); }
      .swapdex-profile-usage { display: flex; align-items: center; flex-wrap: wrap; gap: 0.35rem; margin-top: 0.25rem; }
      .swapdex-profile-usage-meter { display: inline-flex; align-items: center; gap: 0.2rem; }
      .swapdex-profile-reset-advice { flex: 0 0 100%; margin-top: 0.125rem; color: color-mix(in srgb, currentColor 58%, transparent); font-size: 0.625rem; line-height: 0.875rem; }
      .swapdex-profile-reset-advice::after { content: attr(data-swapdex-profile-reset-advice-text); }
      .swapdex-profile-usage-meter::before { content: attr(data-swapdex-profile-usage-label); color: color-mix(in srgb, currentColor 62%, transparent); font-size: 0.625rem; }
      .swapdex-profile-usage-meter::after { content: attr(data-swapdex-profile-usage-value); min-width: 2.1rem; color: color-mix(in srgb, currentColor 72%, transparent); font-size: 0.625rem; font-variant-numeric: tabular-nums; }
      .swapdex-profile-usage-track { display: inline-block; width: 2rem; height: 0.25rem; overflow: hidden; border-radius: 999px; background: color-mix(in srgb, currentColor 14%, transparent); }
      .swapdex-profile-usage-fill { display: block; width: 0; height: 100%; border-radius: inherit; background: var(--swapdex-accent, #3b82f6); transition: width 160ms ease; }
      .swapdex-account-credits { flex: 0 0 auto; border-radius: 999px; padding: 0.0625rem 0.375rem; background: color-mix(in srgb, var(--swapdex-accent, #3b82f6) 14%, transparent); color: var(--swapdex-accent, #3b82f6); font-weight: 500; }
      .swapdex-account-resets { flex: 0 0 auto; border-radius: 999px; padding: 0.0625rem 0.375rem; background: color-mix(in srgb, currentColor 10%, transparent); color: color-mix(in srgb, currentColor 72%, transparent); font-weight: 500; }
      .swapdex-account-actions { display: flex; flex: 0 0 auto; align-items: center; align-self: flex-end; gap: 0.5rem; }
      .swapdex-remove-button { flex: 0 0 auto; border: 1px solid color-mix(in srgb, var(--swapdex-danger, #d92d20) 32%, transparent); border-radius: 0.5rem; padding: 0.375rem 0.75rem; color: var(--swapdex-danger, #d92d20); background: color-mix(in srgb, var(--swapdex-danger, #d92d20) 8%, transparent); cursor: pointer; }
      .swapdex-remove-button:hover { background: color-mix(in srgb, var(--swapdex-danger, #d92d20) 16%, transparent); }
      .swapdex-remove-button:focus-visible { outline: 2px solid color-mix(in srgb, var(--swapdex-danger, #d92d20) 70%, transparent); outline-offset: 2px; }
      .swapdex-remove-button:disabled { cursor: not-allowed; opacity: 0.5; }
      .swapdex-account-row:first-of-type { padding-top: 0; border-top: 0; }
      .swapdex-account-row:last-child { padding-bottom: 0; }
      .swapdex-maintenance-button:disabled { cursor: not-allowed; opacity: 0.5; }
      html[data-swapdex-blur-profile-names="true"] [data-swapdex-profile-name] { filter: blur(5px); user-select: none; }
      @media (prefers-reduced-motion: reduce) { [data-swapdex-profile-name], .swapdex-switch-thumb, .swapdex-interval-chevron, .swapdex-usage-fill, .swapdex-profile-usage-fill { transition: none !important; } }
    `;
    (document.head || document.documentElement).append(style);
  };

  const findSettingsContext = () => {
    if (!supportedLanguage) {
      return null;
    }
    const sidebarButton = label => Array.from(document.querySelectorAll(`button.sidebar-item[aria-label="${label}"]`)).find(visibleElement);
    const archived = sidebarButton("Archived chats");
    const general = sidebarButton("General");
    let nativePage = settingsState.nativePage;
    if (!(nativePage instanceof HTMLElement) || !nativePage.isConnected || nativePage.hasAttribute("data-swapdex-settings-page")) {
      nativePage = Array.from(document.querySelectorAll("div")).find(element => element.classList.contains("group/settings") && !element.hasAttribute("data-swapdex-settings-page") && element.isConnected && element.querySelector("h1")) || null;
    }
    const navigation = archived?.closest("nav.sidebar-navigation");
    if (!archived || !general || !nativePage || !navigation || general.closest("nav.sidebar-navigation") !== navigation) {
      return null;
    }
    const navigationButtons = Array.from(navigation.querySelectorAll("button.sidebar-item")).filter(visibleElement);
    const nativeButtons = navigationButtons.filter(button => !button.hasAttribute("data-swapdex-settings-nav"));
    const selectedButton = nativeButtons.find(button => button.getAttribute("aria-current") === "page");
    const idleButton = nativeButtons.find(button => button !== selectedButton);
    const scroll = nativePage.parentElement;
    if (!scroll || !scroll.contains(nativePage)) {
      return null;
    }
    return {
      archived,
      general,
      nativePage,
      scroll,
      idleClass: idleButton?.className || settingsState.idleClass || archived.className,
      selectedClass: selectedButton?.className || settingsState.selectedClass || general.className
    };
  };

  const paletteColor = (element, fallback) => {
    if (!(element instanceof HTMLElement)) {
      return fallback;
    }
    const color = getComputedStyle(element).backgroundColor;
    return color && color !== "rgba(0, 0, 0, 0)" && color !== "transparent" ? color : fallback;
  };

  const updateToggleVisual = (toggle, checked = blurProfileNames) => {
    if (!(toggle instanceof HTMLElement)) {
      return;
    }
    const accent = settingsState.accentColor || "#3b82f6";
    const off = settingsState.offColor || "rgba(255, 255, 255, 0.1)";
    toggle.style.setProperty("--swapdex-accent", accent);
    toggle.style.backgroundColor = checked ? accent : off;
    const thumb = toggle.querySelector(".swapdex-switch-thumb");
    if (thumb instanceof HTMLElement) {
      thumb.style.backgroundColor = settingsState.thumbColor || "#ffffff";
      thumb.style.transform = "translateY(-50%) " + (checked ? "translateX(0.75rem)" : "translateX(0)");
    }
  };

  const refreshSwitchPalette = context => {
    const nativeSwitches = Array.from(context.nativePage.querySelectorAll('button[role="switch"]'));
    const checked = nativeSwitches.find(element => !element.hasAttribute("disabled") && element.getAttribute("data-state") === "checked");
    const unchecked = nativeSwitches.find(element => !element.hasAttribute("disabled") && element.getAttribute("data-state") === "unchecked");
    const accentElement = checked?.firstElementChild || Array.from(document.querySelectorAll(".bg-chart-blue")).find(element => visibleElement(element));
    const offElement = unchecked?.firstElementChild;
    const thumbElement = checked?.firstElementChild?.firstElementChild;
    const accent = paletteColor(accentElement, "");
    const off = paletteColor(offElement, "");
    const thumb = thumbElement instanceof HTMLElement ? getComputedStyle(thumbElement).backgroundColor : "";
    if (accent.length > 0) {
      settingsState.accentColor = accent;
    }
    if (off.length > 0) {
      settingsState.offColor = off;
    }
    if (thumb.length > 0) {
      settingsState.thumbColor = thumb;
    }
    if (!settingsState.accentColor) {
      settingsState.accentColor = "#3b82f6";
    }
    if (!settingsState.offColor) {
      settingsState.offColor = "rgba(255, 255, 255, 0.1)";
    }
    if (!settingsState.thumbColor) {
      settingsState.thumbColor = "#ffffff";
    }
    updateToggleVisual(settingsState.page?.querySelector("[data-swapdex-privacy-toggle]"));
  };

  const createMaintenanceSwitch = () => {
    const toggle = makeElement("button", "swapdex-switch");
    toggle.type = "button";
    toggle.dataset.swapdexMaintenanceToggle = "true";
    toggle.setAttribute("role", "switch");
    toggle.setAttribute("aria-checked", String(maintenanceEnabled));
    toggle.setAttribute("aria-labelledby", "swapdex-maintenance-label");
    toggle.setAttribute("aria-describedby", "swapdex-maintenance-description");
    toggle.style.cssText = "position:relative;display:inline-flex;width:2rem;height:1.25rem;padding:0;flex-shrink:0;border:0;border-radius:999px;cursor:pointer;";
    const thumb = makeElement("span", "swapdex-switch-thumb");
    thumb.style.cssText = "position:absolute;top:50%;left:0.125rem;width:1rem;height:1rem;border-radius:999px;background:" + (settingsState.thumbColor || "#ffffff") + ";transition:transform 120ms ease;";
    toggle.append(thumb);
    updateToggleVisual(toggle, maintenanceEnabled);
    return toggle;
  };

  const createMenuVisibilityToggle = item => {
    const toggle = makeElement("button", "swapdex-switch");
    toggle.type = "button";
    toggle.dataset.swapdexMenuVisibilityToggle = item.id;
    toggle.setAttribute("role", "switch");
    toggle.setAttribute("aria-checked", String(!hiddenNativeProfileMenuRows.has(item.id)));
    toggle.setAttribute("aria-labelledby", `swapdex-menu-label-${item.id}`);
    toggle.setAttribute("aria-describedby", `swapdex-menu-description-${item.id}`);
    toggle.style.cssText = "position:relative;display:inline-flex;width:2rem;height:1.25rem;padding:0;flex-shrink:0;border:0;border-radius:999px;cursor:pointer;";
    toggle.style.backgroundColor = hiddenNativeProfileMenuRows.has(item.id) ? settingsState.offColor : settingsState.accentColor;
    const thumb = makeElement("span", "swapdex-switch-thumb");
    thumb.style.cssText = "position:absolute;top:50%;left:0.125rem;width:1rem;height:1rem;border-radius:999px;background:" + (settingsState.thumbColor || "#ffffff") + ";transition:transform 120ms ease;";
    thumb.style.transform = "translateY(-50%) " + (hiddenNativeProfileMenuRows.has(item.id) ? "translateX(0)" : "translateX(0.75rem)");
    toggle.append(thumb);
    updateToggleVisual(toggle, !hiddenNativeProfileMenuRows.has(item.id));
    return toggle;
  };

  const setSettingsButtonState = (button, active, context) => {
    if (!button || !context) {
      return;
    }
    const nextClass = active ? settingsState.selectedClass || context.selectedClass : settingsState.idleClass || context.idleClass;
    if (button.className !== nextClass) {
      button.className = nextClass;
    }
    if (active) {
      button.setAttribute("aria-current", "page");
    } else {
      button.removeAttribute("aria-current");
    }
  };

  const clearNativeSidebarSelection = context => {
    for (const button of Array.from(document.querySelectorAll("button.sidebar-item"))) {
      if (button === settingsState.navButton || !visibleElement(button)) {
        continue;
      }
      button.removeAttribute("aria-current");
    }
  };

  const isMeaningfulProfile = profile => {
    if (profile.authenticated !== false) {
      return true;
    }
    const label = normalize(profile.label).toLowerCase();
    return Boolean(normalize(profile.email) || (label && label !== "new account" && label !== "account"));
  };

  const renderAccountList = page => {
    const list = page.querySelector("[data-swapdex-account-list]");
    if (!(list instanceof HTMLElement)) {
      return;
    }
    const profiles = Array.isArray(snapshot.profiles) ? snapshot.profiles : [];
    const visibleIds = new Set();
    for (const profile of profiles) {
      if (!profile || typeof profile.id !== "string" || !/^[A-Za-z0-9_-]{1,64}$/.test(profile.id) || !isMeaningfulProfile(profile)) {
        continue;
      }
      visibleIds.add(profile.id);
      let row = list.querySelector(`[data-swapdex-account-row="${CSS.escape(profile.id)}"]`);
      if (!(row instanceof HTMLElement)) {
        row = makeElement("div", "swapdex-account-row");
        row.dataset.swapdexAccountRow = profile.id;
        const identity = makeElement("div", "swapdex-account-identity");
        const name = makeElement("span", "text-sm text-default");
        name.dataset.swapdexAccountName = "true";
        markProfileName(name);
        const meta = makeElement("div", "swapdex-account-meta");
        const avatar = makeElement("span", "swapdex-account-avatar");
        avatar.dataset.swapdexAccountAvatar = "true";
        avatar.setAttribute("aria-hidden", "true");
        const avatarImage = document.createElement("img");
        avatarImage.className = "swapdex-account-avatar-image";
        avatarImage.dataset.swapdexAccountAvatarImage = "true";
        avatarImage.alt = "";
        avatarImage.setAttribute("aria-hidden", "true");
        avatarImage.draggable = false;
        const avatarFallback = makeElement("span", "swapdex-account-avatar-fallback", "A");
        avatarFallback.dataset.swapdexAccountAvatarFallback = "true";
        avatar.append(avatarImage, avatarFallback);
        const detail = makeElement("span", "text-xs text-codex-description swapdex-account-detail");
        detail.dataset.swapdexAccountDetail = "true";
        markProfileName(detail);
        const divider = makeElement("span", "swapdex-account-divider");
        divider.setAttribute("aria-hidden", "true");
        const lastWarmed = makeElement("span", "text-xs text-codex-description swapdex-account-last-warmed");
        lastWarmed.dataset.swapdexAccountLastWarmed = "true";
        lastWarmed.setAttribute("role", "status");
        lastWarmed.setAttribute("aria-live", "polite");
        const insights = makeElement("div", "swapdex-account-insights");
        insights.dataset.swapdexAccountInsights = "true";
        const usage = makeElement("span", "swapdex-account-usage");
        usage.dataset.swapdexAccountUsage = "true";
        const usageLabel = makeElement("span", "swapdex-account-usage-label", "Usage");
        const primaryUsage = createUsageMeter("primary", "5h");
        const secondaryUsage = createUsageMeter("secondary", "7d");
        usage.append(usageLabel, primaryUsage, secondaryUsage);
        const insightsDivider = makeElement("span", "swapdex-account-insights-divider");
        insightsDivider.dataset.swapdexAccountInsightsDivider = "true";
        insightsDivider.setAttribute("aria-hidden", "true");
        const credits = makeElement("span", "swapdex-account-credits");
        credits.dataset.swapdexAccountCredits = "true";
        const resetsDivider = makeElement("span", "swapdex-account-insights-divider");
        resetsDivider.dataset.swapdexAccountResetsDivider = "true";
        resetsDivider.setAttribute("aria-hidden", "true");
        const resets = makeElement("span", "swapdex-account-resets");
        resets.dataset.swapdexAccountResets = "true";
        const resetAdvice = makeElement("span", "swapdex-account-reset-advice");
        resetAdvice.dataset.swapdexAccountResetAdvice = "true";
        insights.append(usage, insightsDivider, credits, resetsDivider, resets, resetAdvice);
        meta.append(avatar, detail, divider, lastWarmed);
        identity.append(name, meta, insights);
        const actions = makeElement("div", "swapdex-account-actions");
        const action = makeElement("button", "swapdex-maintenance-button text-xs", "Warm up");
        action.type = "button";
        action.dataset.swapdexAccountWarmup = profile.id;
        const remove = makeElement("button", "swapdex-remove-button text-xs", "Remove");
        remove.type = "button";
        remove.dataset.swapdexAccountRemove = profile.id;
        actions.append(action, remove);
        row.append(identity, actions);
        list.append(row);
      }
      const name = row.querySelector("[data-swapdex-account-name]");
      const avatar = row.querySelector("[data-swapdex-account-avatar]");
      const detail = row.querySelector("[data-swapdex-account-detail]");
      const lastWarmed = row.querySelector("[data-swapdex-account-last-warmed]");
      const insights = row.querySelector("[data-swapdex-account-insights]");
      const usage = row.querySelector("[data-swapdex-account-usage]");
      const primaryUsage = row.querySelector('[data-swapdex-usage-meter="primary"]');
      const secondaryUsage = row.querySelector('[data-swapdex-usage-meter="secondary"]');
      const insightsDivider = row.querySelector("[data-swapdex-account-insights-divider]");
      const credits = row.querySelector("[data-swapdex-account-credits]");
      const resetsDivider = row.querySelector("[data-swapdex-account-resets-divider]");
      const resets = row.querySelector("[data-swapdex-account-resets]");
      const resetAdvice = row.querySelector("[data-swapdex-account-reset-advice]");
      const action = row.querySelector("[data-swapdex-account-warmup], [data-swapdex-account-reauth]");
      const remove = row.querySelector("[data-swapdex-account-remove]");
      if (!(name instanceof HTMLElement) || !(avatar instanceof HTMLElement) || !(detail instanceof HTMLElement) || !(lastWarmed instanceof HTMLElement) || !(insights instanceof HTMLElement) || !(usage instanceof HTMLElement) || !(primaryUsage instanceof HTMLElement) || !(secondaryUsage instanceof HTMLElement) || !(insightsDivider instanceof HTMLElement) || !(credits instanceof HTMLElement) || !(resetsDivider instanceof HTMLElement) || !(resets instanceof HTMLElement) || !(resetAdvice instanceof HTMLElement) || !(action instanceof HTMLButtonElement) || !(remove instanceof HTMLButtonElement)) {
        continue;
      }
      const accountName = normalize(profile.label) || normalize(profile.email) || "Account";
      const needsReauth = profile.authenticated === false;
      if (!needsReauth) {
        pendingReauths.delete(profile.id);
      }
      const reauthPending = needsReauth && pendingReauths.has(profile.id);
      const accountDetail = needsReauth ? (normalize(profile.email) || "Reauthentication required") : (normalize(profile.email) || normalize(profile.plan) || "No account details");
      applySettingsAvatar(avatar, profile);
      setElementText(name, accountName);
      setElementText(detail, accountDetail);
      const primaryUsageVisible = updateUsageMeter(primaryUsage, profile.primary_remaining, "5h");
      const secondaryUsageVisible = updateUsageMeter(secondaryUsage, profile.secondary_remaining, "7d");
      const usageVisible = primaryUsageVisible || secondaryUsageVisible;
      const creditsText = accountCreditsText(profile);
      const resetsText = accountResetsText(profile);
      const accountResetAdviceText = resetAdviceText(profile);
      setElementText(credits, creditsText);
      setElementText(resets, resetsText);
      setElementText(resetAdvice, accountResetAdviceText);
      usage.hidden = !usageVisible;
      credits.hidden = creditsText.length === 0;
      resets.hidden = resetsText.length === 0;
      resetAdvice.hidden = accountResetAdviceText.length === 0;
      insightsDivider.hidden = !usageVisible || (creditsText.length === 0 && resetsText.length === 0 && accountResetAdviceText.length === 0);
      resetsDivider.hidden = creditsText.length === 0 || resetsText.length === 0;
      insights.hidden = !usageVisible && creditsText.length === 0 && resetsText.length === 0 && accountResetAdviceText.length === 0;
      delete action.dataset.swapdexAccountWarmup;
      delete action.dataset.swapdexAccountReauth;
      action.dataset.swapdexAccountWarmup = needsReauth ? "" : profile.id;
      if (needsReauth) {
        delete action.dataset.swapdexAccountWarmup;
        action.dataset.swapdexAccountReauth = profile.id;
      }
      const lastMaintenanceAt = Number(profile.last_maintenance_at);
      const pendingStartedAt = pendingWarmups.get(profile.id);
      if (!needsReauth && pendingStartedAt !== undefined && Number.isFinite(lastMaintenanceAt) && lastMaintenanceAt > pendingStartedAt) {
        pendingWarmups.delete(profile.id);
      }
      const pending = !needsReauth && pendingWarmups.has(profile.id);
      if (needsReauth) {
        const history = Number.isFinite(lastMaintenanceAt) && lastMaintenanceAt > 0 ? `Last warmed up ${new Date(lastMaintenanceAt * 1000).toLocaleString()}` : "Never warmed up";
        setElementText(lastWarmed, `${history} · Reauthentication needed`);
      } else if (pending) {
        setElementText(lastWarmed, "Warming up…");
      } else if (Number.isFinite(lastMaintenanceAt) && lastMaintenanceAt > 0) {
        const suffix = profile.maintenance_status === "error" ? " · failed" : "";
        setElementText(lastWarmed, `Last warmed up ${new Date(lastMaintenanceAt * 1000).toLocaleString()}${suffix}`);
      } else {
        setElementText(lastWarmed, "Not warmed up yet");
      }
      action.disabled = pending || reauthPending;
      setElementText(action, needsReauth ? (reauthPending ? "Reauthenticating…" : "Reauthenticate") : (pending ? "Warming up…" : "Warm up"));
      action.setAttribute("aria-label", needsReauth ? `Reauthenticate ${accountName}` : `Warm up ${accountName}`);
      const removing = pendingRemovals.has(profile.id);
      remove.disabled = removing;
      setElementText(remove, removing ? "Removing…" : "Remove");
      remove.setAttribute("aria-label", removing ? `Removing ${accountName}` : `Remove ${accountName} and its stored credentials`);
    }
    const empty = list.querySelector("[data-swapdex-account-empty]");
    if (empty instanceof HTMLElement) {
      empty.hidden = visibleIds.size > 0;
    }
    for (const row of Array.from(list.querySelectorAll("[data-swapdex-account-row]"))) {
      const id = row instanceof HTMLElement ? row.dataset.swapdexAccountRow : "";
      if (!visibleIds.has(id)) {
        row.remove();
        pendingRemovals.delete(id);
      }
    }
  };

  const maintenanceIntervalText = value => `${value} hours`;
  const createIntervalChevron = () => {
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("class", "swapdex-interval-chevron");
    svg.setAttribute("data-swapdex-interval-chevron", "true");
    svg.setAttribute("viewBox", "0 0 20 21");
    svg.setAttribute("width", "16");
    svg.setAttribute("height", "16");
    svg.setAttribute("fill", "none");
    svg.setAttribute("aria-hidden", "true");
    const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("d", "M5.5 8L10 12.5L14.5 8");
    path.setAttribute("stroke", "currentColor");
    path.setAttribute("stroke-width", "1.5");
    path.setAttribute("stroke-linecap", "round");
    path.setAttribute("stroke-linejoin", "round");
    svg.append(path);
    return svg;
  };
  const positionIntervalMenu = () => {
    if (!(intervalMenu instanceof HTMLElement) || !(intervalMenuButton instanceof HTMLElement)) {
      return;
    }
    const buttonRect = intervalMenuButton.getBoundingClientRect();
    const minimumWidth = Math.max(buttonRect.width, 144);
    intervalMenu.style.minWidth = `${minimumWidth}px`;
    const menuWidth = Math.max(intervalMenu.offsetWidth, minimumWidth);
    const menuHeight = intervalMenu.offsetHeight;
    const halfWidth = menuWidth / 2;
    const centeredLeft = buttonRect.left + buttonRect.width / 2;
    const left = Math.max(8 + halfWidth, Math.min(window.innerWidth - 8 - halfWidth, centeredLeft));
    const gap = 4;
    const edge = 8;
    const spaceBelow = window.innerHeight - buttonRect.bottom - gap - edge;
    const spaceAbove = buttonRect.top - gap - edge;
    const placeAbove = spaceBelow < menuHeight && spaceAbove >= spaceBelow;
    intervalMenu.dataset.swapdexPlacement = placeAbove ? "top" : "bottom";
    intervalMenu.style.left = `${left}px`;
    intervalMenu.style.top = `${placeAbove ? Math.max(edge, buttonRect.top - menuHeight - gap) : Math.min(window.innerHeight - menuHeight - edge, buttonRect.bottom + gap)}px`;
    const chevron = intervalMenuButton.querySelector("[data-swapdex-interval-chevron]");
    if (chevron instanceof SVGElement) {
      chevron.style.transform = placeAbove ? "rotate(180deg)" : "rotate(0deg)";
    }
  };
  const closeIntervalMenu = (restoreFocus = true) => {
    if (!(intervalMenu instanceof HTMLElement)) {
      return;
    }
    const button = intervalMenuButton;
    intervalMenu.remove();
    intervalMenu = null;
    intervalMenuButton = null;
    window.removeEventListener("resize", positionIntervalMenu);
    window.removeEventListener("scroll", positionIntervalMenu, true);
    if (button instanceof HTMLElement && button.isConnected) {
      button.setAttribute("aria-expanded", "false");
      button.setAttribute("data-state", "closed");
      const chevron = button.querySelector("[data-swapdex-interval-chevron]");
      if (chevron instanceof SVGElement) {
        chevron.style.transform = "rotate(0deg)";
      }
      if (restoreFocus) {
        button.focus({ preventScroll: true });
      }
    }
  };
  const selectMaintenanceInterval = value => {
    if (!validMaintenanceIntervals.includes(value)) {
      return;
    }
    if (value === maintenanceIntervalHours) {
      closeIntervalMenu(true);
      return;
    }
    maintenanceIntervalHours = value;
    writePreferences();
    queueMaintenanceConfig();
    closeIntervalMenu(true);
    updateSettingsPage();
  };
  const openIntervalMenu = button => {
    if (!(button instanceof HTMLButtonElement) || button.disabled) {
      return;
    }
    if (intervalMenuButton === button) {
      closeIntervalMenu(true);
      return;
    }
    closeIntervalMenu(false);
    intervalMenuButton = button;
    const menu = makeElement("div", "swapdex-interval-menu");
    menu.id = "swapdex-interval-menu";
    menu.setAttribute("role", "menu");
    menu.setAttribute("aria-label", "Warm-up interval");
    menu.dataset.swapdexIntervalMenu = "true";
    for (const value of validMaintenanceIntervals) {
      const option = makeElement("button", "swapdex-interval-menu-item text-sm");
      option.type = "button";
      option.setAttribute("role", "menuitemradio");
      option.setAttribute("aria-checked", String(value === maintenanceIntervalHours));
      option.dataset.swapdexIntervalOption = String(value);
      const text = makeElement("span", "", maintenanceIntervalText(value));
      const check = makeElement("span", "swapdex-interval-menu-check", value === maintenanceIntervalHours ? "✓" : "");
      check.setAttribute("aria-hidden", "true");
      option.append(text, check);
      menu.append(option);
    }
    document.body.append(menu);
    intervalMenu = menu;
    button.setAttribute("aria-expanded", "true");
    button.setAttribute("aria-controls", menu.id);
    button.setAttribute("data-state", "open");
    window.addEventListener("resize", positionIntervalMenu);
    window.addEventListener("scroll", positionIntervalMenu, true);
    positionIntervalMenu();
    const selected = menu.querySelector('[aria-checked="true"]');
    if (selected instanceof HTMLElement) {
      selected.focus({ preventScroll: true });
    }
  };

  const createSettingsPage = context => {
    const page = makeElement("div", "swapdex-settings-page group/settings mx-auto flex w-full flex-col max-w-3xl electron:min-w-[min(100%,calc(320px*var(--codex-window-zoom)))]");
    page.id = settingsPageId;
    page.dataset.swapdexSettingsPage = "true";
    page.hidden = true;

    const nativeHeader = context.nativePage.querySelector("header");
    const nativeHeading = context.nativePage.querySelector("h1");
    const header = makeElement("header", `swapdex-settings-header ${nativeHeader?.className || "flex flex-col gap-4 px-[var(--detail-page-inline-inset,0px)]"}`);
    const heading = makeElement("h1", nativeHeading?.className || "min-w-0 break-words text-default heading-lg font-normal", "Swapdex");
    const description = makeElement("p", "text-sm text-codex-description", "Manage local account privacy, profile menu actions, and switching preferences.");
    const preferenceStatus = makeElement("p", "text-xs text-codex-description", "Preferences save automatically for this Codex profile.");
    preferenceStatus.dataset.swapdexPreferencesStatus = "true";
    preferenceStatus.setAttribute("role", "status");
    preferenceStatus.setAttribute("aria-live", "polite");
    header.append(heading, description, preferenceStatus);

    const tabList = makeElement("div", "swapdex-settings-tabs flex items-center px-[var(--detail-page-inline-inset,0px)]");
    tabList.setAttribute("role", "tablist");
    tabList.setAttribute("aria-label", "Swapdex settings");
    const privacyTab = makeElement("button", "swapdex-settings-tab", "Privacy");
    privacyTab.type = "button";
    privacyTab.id = "swapdex-settings-tab-privacy";
    privacyTab.dataset.swapdexSettingsTab = "privacy";
    privacyTab.setAttribute("role", "tab");
    privacyTab.setAttribute("aria-controls", "swapdex-settings-panel-privacy");
    const accountsTab = makeElement("button", "swapdex-settings-tab", "Accounts");
    accountsTab.type = "button";
    accountsTab.id = "swapdex-settings-tab-accounts";
    accountsTab.dataset.swapdexSettingsTab = "accounts";
    accountsTab.setAttribute("role", "tab");
    accountsTab.setAttribute("aria-controls", "swapdex-settings-panel-accounts");
    const menuTab = makeElement("button", "swapdex-settings-tab", "Menu");
    menuTab.type = "button";
    menuTab.id = "swapdex-settings-tab-menu";
    menuTab.dataset.swapdexSettingsTab = "menu";
    menuTab.setAttribute("role", "tab");
    menuTab.setAttribute("aria-controls", "swapdex-settings-panel-menu");
    const privacyPanel = makeElement("section", "swapdex-settings-panel flex flex-col gap-6 px-[var(--detail-page-inline-inset,0px)] py-6");
    privacyPanel.id = "swapdex-settings-panel-privacy";
    privacyPanel.dataset.swapdexSettingsPanel = "privacy";
    privacyPanel.setAttribute("role", "tabpanel");
    privacyPanel.setAttribute("aria-labelledby", privacyTab.id);
    const privacyHeading = makeElement("h2", "swapdex-settings-section-title heading-md font-normal", "Privacy");
    const privacyDescription = makeElement("p", "text-sm text-codex-description", "Control what profile names are visible in the Codex account menu.");
    const privacyCard = makeElement("section", "swapdex-settings-card flex flex-col gap-4 rounded-xl");
    const privacyRow = makeElement("div", "flex items-center justify-between gap-6");
    const privacyCopy = makeElement("div", "flex min-w-0 flex-col gap-1");
    const privacyLabel = makeElement("span", "text-sm text-default", "Blur profile names");
    privacyLabel.id = "swapdex-privacy-label";
    const privacyHelp = makeElement("span", "text-sm text-codex-description", "Keeps profile names visually obscured while preserving keyboard and screen-reader access.");
    privacyHelp.id = "swapdex-privacy-description";
    privacyCopy.append(privacyLabel, privacyHelp);
    const nativeSwitches = Array.from(context.nativePage.querySelectorAll('button[role="switch"]'));
    const checkedNativeSwitch = nativeSwitches.find(element => !element.hasAttribute("disabled") && element.getAttribute("data-state") === "checked");
    const uncheckedNativeSwitch = nativeSwitches.find(element => !element.hasAttribute("disabled") && element.getAttribute("data-state") === "unchecked");
    const nativeAccent = checkedNativeSwitch?.firstElementChild ? getComputedStyle(checkedNativeSwitch.firstElementChild).backgroundColor : "";
    const nativeOff = uncheckedNativeSwitch?.firstElementChild ? getComputedStyle(uncheckedNativeSwitch.firstElementChild).backgroundColor : "rgba(255, 255, 255, 0.1)";
    const nativeThumb = checkedNativeSwitch?.firstElementChild?.firstElementChild ? getComputedStyle(checkedNativeSwitch.firstElementChild.firstElementChild).backgroundColor : "#ffffff";
    settingsState.accentColor = nativeAccent || settingsState.accentColor || "#3b82f6";
    settingsState.offColor = nativeOff || settingsState.offColor || "rgba(255, 255, 255, 0.1)";
    settingsState.thumbColor = nativeThumb || settingsState.thumbColor || "#ffffff";
    const toggle = makeElement("button", "swapdex-switch");
    toggle.type = "button";
    toggle.dataset.swapdexPrivacyToggle = "true";
    toggle.setAttribute("role", "switch");
    toggle.setAttribute("aria-checked", String(blurProfileNames));
    toggle.setAttribute("aria-labelledby", privacyLabel.id);
    toggle.setAttribute("aria-describedby", privacyHelp.id);
    toggle.style.cssText = "position:relative;display:inline-flex;width:2rem;height:1.25rem;padding:0;flex-shrink:0;border:0;border-radius:999px;cursor:pointer;";
    toggle.style.backgroundColor = blurProfileNames ? settingsState.accentColor : settingsState.offColor;
    const thumb = makeElement("span", "swapdex-switch-thumb");
    thumb.style.cssText = "position:absolute;top:50%;left:0.125rem;width:1rem;height:1rem;border-radius:999px;background:" + nativeThumb + ";transition:transform 120ms ease;";
    thumb.style.transform = "translateY(-50%) " + (blurProfileNames ? "translateX(0.75rem)" : "translateX(0)");
    toggle.append(thumb);
    updateToggleVisual(toggle);
    const toggleControl = makeElement("div", "flex shrink-0 items-center");
    toggleControl.append(toggle);
    privacyRow.append(privacyCopy, toggleControl);
    const privacyNote = makeElement("p", "text-xs text-codex-description", "This preference is stored locally in this Codex profile.");
    privacyCard.append(privacyRow, privacyNote);
    privacyPanel.append(privacyHeading, privacyDescription, privacyCard);

    const accountsPanel = makeElement("section", "swapdex-settings-panel flex flex-col gap-4 px-[var(--detail-page-inline-inset,0px)] py-4");
    accountsPanel.id = "swapdex-settings-panel-accounts";
    accountsPanel.dataset.swapdexSettingsPanel = "accounts";
    accountsPanel.setAttribute("role", "tabpanel");
    accountsPanel.setAttribute("aria-labelledby", accountsTab.id);
    const accountsHeading = makeElement("h2", "swapdex-settings-section-title heading-md font-normal", "Accounts");
    const accountsDescription = makeElement("p", "text-sm text-codex-description", "Keep stored Codex credentials ready for switching without sending chats or model requests.");
    const accountListCard = makeElement("section", "swapdex-settings-card flex flex-col gap-3 rounded-xl");
    const accountListHeader = makeElement("div", "flex items-center justify-between gap-4");
    const accountListHeading = makeElement("h3", "text-sm text-default", "Stored accounts");
    const runNow = makeElement("button", "swapdex-maintenance-button text-xs", "Warm up all");
    runNow.type = "button";
    runNow.dataset.swapdexMaintenanceRun = "true";
    accountListHeader.append(accountListHeading, runNow);
    const accountListDescription = makeElement("p", "text-xs text-codex-description", "Warm up an individual account without sending chats or model requests. Removing an account asks for confirmation and deletes its stored credentials; removing the signed-in account signs it out.");
    const accountList = makeElement("div", "swapdex-account-list");
    accountList.dataset.swapdexAccountList = "true";
    accountList.setAttribute("aria-label", "Stored accounts");
    const accountListEmpty = makeElement("p", "text-sm text-codex-description", "No saved accounts yet.");
    accountListEmpty.dataset.swapdexAccountEmpty = "true";
    accountList.append(accountListEmpty);
    accountListCard.append(accountListHeader, accountListDescription, accountList);
    const maintenanceCard = makeElement("section", "swapdex-settings-card flex flex-col gap-3 rounded-xl");
    const maintenanceRow = makeElement("div", "flex items-center justify-between gap-6");
    const maintenanceCopy = makeElement("div", "flex min-w-0 flex-col gap-1");
    const maintenanceLabel = makeElement("span", "text-sm text-default", "Keep accounts active");
    maintenanceLabel.id = "swapdex-maintenance-label";
    const maintenanceDescription = makeElement("span", "text-sm text-codex-description", "Refreshes stored Codex credentials while Swapdex is running. No chats or model requests are sent.");
    maintenanceDescription.id = "swapdex-maintenance-description";
    maintenanceCopy.append(maintenanceLabel, maintenanceDescription);
    const maintenanceControl = makeElement("div", "flex shrink-0 items-center");
    const maintenanceToggle = createMaintenanceSwitch();
    maintenanceControl.append(maintenanceToggle);
    maintenanceRow.append(maintenanceCopy, maintenanceControl);
    const intervalRow = makeElement("div", "flex items-center justify-between gap-6 text-sm");
    const intervalLabel = makeElement("span", "text-default", "Warm up every");
    intervalLabel.id = "swapdex-maintenance-interval-label";
    const intervalButton = makeElement("button", "no-drag swapdex-interval-trigger");
    intervalButton.type = "button";
    intervalButton.dataset.swapdexMaintenanceInterval = "true";
    intervalButton.setAttribute("aria-haspopup", "menu");
    intervalButton.setAttribute("aria-expanded", "false");
    intervalButton.setAttribute("data-state", "closed");
    const intervalValue = makeElement("span", "flex min-w-0 flex-1 items-center", maintenanceIntervalText(maintenanceIntervalHours));
    intervalValue.id = "swapdex-maintenance-interval-value";
    intervalValue.dataset.swapdexMaintenanceIntervalValue = "true";
    intervalButton.setAttribute("aria-labelledby", `${intervalLabel.id} ${intervalValue.id}`);
    intervalButton.append(intervalValue, createIntervalChevron());
    intervalButton.disabled = !maintenanceEnabled;
    intervalRow.append(intervalLabel, intervalButton);
    const maintenanceStatus = makeElement("p", "text-xs text-codex-description", "No warm-up recorded yet");
    maintenanceStatus.dataset.swapdexMaintenanceStatus = "true";
    maintenanceStatus.setAttribute("role", "status");
    maintenanceStatus.setAttribute("aria-live", "polite");
    maintenanceCard.append(maintenanceRow, intervalRow, maintenanceStatus);
    accountsPanel.append(accountsHeading, accountsDescription, accountListCard, maintenanceCard);

    const menuPanel = makeElement("section", "swapdex-settings-panel flex flex-col gap-4 px-[var(--detail-page-inline-inset,0px)] py-4");
    menuPanel.id = "swapdex-settings-panel-menu";
    menuPanel.dataset.swapdexSettingsPanel = "menu";
    menuPanel.setAttribute("role", "tabpanel");
    menuPanel.setAttribute("aria-labelledby", menuTab.id);
    const menuHeading = makeElement("h2", "swapdex-settings-section-title heading-md font-normal", "Profile menu");
    const menuDescription = makeElement("p", "text-sm text-codex-description", "Choose which optional actions appear when you open your Codex profile menu.");
    const menuCard = makeElement("section", "swapdex-settings-card flex flex-col rounded-xl");
    const menuList = makeElement("div", "swapdex-menu-settings-list");
    for (const item of optionalMenuItems) {
      const menuRow = makeElement("div", "swapdex-menu-settings-row");
      const copy = makeElement("div", "swapdex-menu-settings-copy");
      const label = makeElement("span", "text-sm text-default", item.title);
      label.id = `swapdex-menu-label-${item.id}`;
      const description = makeElement("span", "text-xs text-codex-description", item.description);
      description.id = `swapdex-menu-description-${item.id}`;
      copy.append(label, description);
      menuRow.append(copy, createMenuVisibilityToggle(item));
      menuList.append(menuRow);
    }
    const menuNote = makeElement("p", "swapdex-menu-settings-note", "Add account and Settings always remain available. Hidden actions return automatically when Codex makes them available again.");
    menuCard.append(menuList, menuNote);
    menuPanel.append(menuHeading, menuDescription, menuCard);

    page.append(header, tabList, privacyPanel, accountsPanel, menuPanel);
    return page;
  };

  const updateSettingsPage = () => {
    const page = settingsState.page;
    if (!(page instanceof HTMLElement) || !page.isConnected) {
      return;
    }
    const tabs = Array.from(page.querySelectorAll('[data-swapdex-settings-tab]'));
    for (const tab of tabs) {
      const selected = tab.dataset.swapdexSettingsTab === settingsState.tab;
      tab.setAttribute("aria-selected", String(selected));
      tab.tabIndex = selected ? 0 : -1;
    }
    for (const panel of Array.from(page.querySelectorAll('[data-swapdex-settings-panel]'))) {
      panel.hidden = panel.dataset.swapdexSettingsPanel !== settingsState.tab;
    }
    const preferenceStatus = page.querySelector("[data-swapdex-preferences-status]");
    if (preferenceStatus) {
      setElementText(preferenceStatus, preferenceSaveState === "saved" ? "Preferences save automatically for this Codex profile." : "Preferences could not be saved in this browser profile.");
      preferenceStatus.dataset.state = preferenceSaveState;
    }
    const toggle = page.querySelector("[data-swapdex-privacy-toggle]");
    if (toggle) {
      toggle.setAttribute("aria-checked", String(blurProfileNames));
      updateToggleVisual(toggle);
    }
    for (const item of optionalMenuItems) {
      const menuToggle = page.querySelector(`[data-swapdex-menu-visibility-toggle="${item.id}"]`);
      if (menuToggle instanceof HTMLElement) {
        const visible = !hiddenNativeProfileMenuRows.has(item.id);
        menuToggle.setAttribute("aria-checked", String(visible));
        updateToggleVisual(menuToggle, visible);
      }
    }
    renderAccountList(page);
    const maintenanceToggle = page.querySelector("[data-swapdex-maintenance-toggle]");
    if (maintenanceToggle) {
      maintenanceToggle.setAttribute("aria-checked", String(maintenanceEnabled));
      updateToggleVisual(maintenanceToggle, maintenanceEnabled);
    }
    const intervalButton = page.querySelector("[data-swapdex-maintenance-interval]");
    if (intervalButton instanceof HTMLButtonElement) {
      const intervalValue = intervalButton.querySelector("[data-swapdex-maintenance-interval-value]");
      if (intervalValue) {
        setElementText(intervalValue, maintenanceIntervalText(maintenanceIntervalHours));
      }
      intervalButton.disabled = !maintenanceEnabled;
      if (maintenanceEnabled === false && intervalMenuButton === intervalButton) {
        closeIntervalMenu(false);
      }
    }
    const maintenanceStatus = page.querySelector("[data-swapdex-maintenance-status]");
    if (maintenanceStatus) {
      const timestamps = Array.isArray(snapshot.profiles) ? snapshot.profiles.map(profile => Number(profile?.last_maintenance_at)).filter(value => Number.isFinite(value) && value > 0) : [];
      const latest = timestamps.length > 0 ? Math.max(...timestamps) : 0;
      const hasError = Array.isArray(snapshot.profiles) && snapshot.profiles.some(profile => profile?.maintenance_status === "error");
      setElementText(maintenanceStatus, latest > 0 ? `Last automatic warm-up ${new Date(latest * 1000).toLocaleString()}${hasError ? " · needs attention" : ""}` : "No automatic warm-up recorded yet");
    }
  };

  const createSettingsButton = context => {
    const button = sanitizeSettingsClone(context.general);
    button.type = "button";
      button.className = settingsState.idleClass || context.idleClass;
    button.dataset.swapdexSettingsNav = "true";
    button.setAttribute("aria-label", "Swapdex");
    button.setAttribute("aria-controls", settingsPageId);
    setNativeButtonText(button, "General", "Swapdex");
    context.archived.parentElement?.insertBefore(button, context.archived.nextSibling);
    return button;
  };

  const closeSettingsPage = (restoreFocus = true) => {
    if (intervalMenu) {
      closeIntervalMenu(false);
    }
    if (!settingsState.active) {
      return;
    }
    settingsState.active = false;
    if (settingsState.nativePage instanceof HTMLElement && settingsState.nativePage.isConnected) {
      settingsState.nativePage.hidden = false;
    }
    if (settingsState.page instanceof HTMLElement && settingsState.page.isConnected) {
      settingsState.page.hidden = true;
    }
    document.documentElement.removeAttribute("data-swapdex-settings-active");
    const context = findSettingsContext();
    setSettingsButtonState(settingsState.navButton, false, context);
    if (restoreFocus && settingsState.navButton instanceof HTMLElement && settingsState.navButton.isConnected) {
      settingsState.navButton.focus({ preventScroll: true });
    }
  };

  const ensureSettingsPage = () => {
    const context = findSettingsContext();
    if (!context) {
      if (settingsState.active) {
        closeSettingsPage(false);
      }
      if (settingsState.page instanceof HTMLElement) {
        settingsState.page.remove();
      }
      if (settingsState.navButton instanceof HTMLElement) {
        settingsState.navButton.remove();
      }
      settingsState.nativePage = null;
      settingsState.scroll = null;
      settingsState.navButton = null;
      settingsState.page = null;
      return null;
    }
    if (settingsState.nativePage instanceof HTMLElement && settingsState.nativePage !== context.nativePage && settingsState.nativePage.isConnected) {
      settingsState.nativePage.hidden = false;
      if (settingsState.active) {
        settingsState.active = false;
        document.documentElement.removeAttribute("data-swapdex-settings-active");
      }
    }
    settingsState.nativePage = context.nativePage;
    settingsState.scroll = context.scroll;
    if (!settingsState.active) {
      settingsState.idleClass = context.idleClass;
      settingsState.selectedClass = context.selectedClass;
    }
    installSettingsStyles();
    refreshSwitchPalette(context);
    if (!(settingsState.navButton instanceof HTMLElement) || !settingsState.navButton.isConnected || settingsState.navButton.parentElement !== context.archived.parentElement) {
      settingsState.navButton?.remove();
      settingsState.navButton = createSettingsButton(context);
    }
    if (!(settingsState.page instanceof HTMLElement) || !settingsState.page.isConnected || settingsState.page.parentElement !== context.scroll) {
      settingsState.page?.remove();
      settingsState.page = createSettingsPage(context);
      context.scroll.insertBefore(settingsState.page, context.nativePage.nextSibling);
      updateSettingsPage();
    }
    if (settingsState.active) {
      context.nativePage.hidden = true;
      settingsState.page.hidden = false;
      document.documentElement.setAttribute("data-swapdex-settings-active", "true");
      clearNativeSidebarSelection(context);
      setSettingsButtonState(settingsState.navButton, true, context);
      updateSettingsPage();
    } else {
      document.documentElement.removeAttribute("data-swapdex-settings-active");
      context.nativePage.hidden = false;
      settingsState.page.hidden = true;
      setSettingsButtonState(settingsState.navButton, false, context);
    }
    return context;
  };

  const openSettingsPage = () => {
    const context = ensureSettingsPage();
    if (!context) {
      return;
    }
    settingsState.active = true;
    ensureSettingsPage();
    const activeTab = settingsState.page?.querySelector(`[data-swapdex-settings-tab="${CSS.escape(settingsState.tab)}"]`);
    if (activeTab instanceof HTMLElement) {
      activeTab.focus({ preventScroll: true });
    }
  };

  const activateSettingsTab = tabName => {
    // A tab is valid when its panel is on the page. This used to be a list of names
    // kept in step by hand, and forgetting to add one left that tab rendering but
    // silently ignoring every click.
    const page = settingsState.page;
    if (!(page instanceof HTMLElement) || !page.isConnected) {
      return;
    }
    if (page.querySelector(`[data-swapdex-settings-panel="${CSS.escape(tabName)}"]`) === null) {
      return;
    }
    settingsState.tab = tabName;
    updateSettingsPage();
    const tab = settingsState.page?.querySelector(`[data-swapdex-settings-tab="${CSS.escape(tabName)}"]`);
    if (tab instanceof HTMLElement) {
      tab.focus({ preventScroll: true });
    }
  };

  const reconcileSettings = () => {
    ensureSettingsPage();
  };

  document.addEventListener("click", event => {
    const target = event.target instanceof Element ? event.target : null;
    if (!target) {
      return;
    }
    if (intervalMenu && !target.closest("[data-swapdex-interval-menu], [data-swapdex-maintenance-interval]")) {
      closeIntervalMenu(false);
    }
    const intervalButton = target.closest("[data-swapdex-maintenance-interval]");
    if (intervalButton instanceof HTMLButtonElement) {
      event.preventDefault();
      event.stopPropagation();
      openIntervalMenu(intervalButton);
      return;
    }
    const intervalOption = target.closest("[data-swapdex-interval-option]");
    if (intervalOption instanceof HTMLButtonElement) {
      event.preventDefault();
      event.stopPropagation();
      selectMaintenanceInterval(Number(intervalOption.dataset.swapdexIntervalOption));
      return;
    }
    const nav = target.closest("[data-swapdex-settings-nav]");
    if (nav instanceof HTMLElement) {
      event.preventDefault();
      event.stopPropagation();
      openSettingsPage();
      return;
    }
    const tab = target.closest("[data-swapdex-settings-tab]");
    if (tab instanceof HTMLElement) {
      event.preventDefault();
      event.stopPropagation();
      activateSettingsTab(tab.dataset.swapdexSettingsTab || "privacy");
      return;
    }
    const toggle = target.closest("[data-swapdex-privacy-toggle]");
    if (toggle instanceof HTMLElement) {
      event.preventDefault();
      event.stopPropagation();
      blurProfileNames = !blurProfileNames;
      writePreferences();
      applyPrivacyPreference();
      updateSettingsPage();
      return;
    }
    const menuVisibilityToggle = target.closest("[data-swapdex-menu-visibility-toggle]");
    if (menuVisibilityToggle instanceof HTMLElement) {
      event.preventDefault();
      event.stopPropagation();
      const id = menuVisibilityToggle.dataset.swapdexMenuVisibilityToggle || "";
      if (!optionalMenuIds.has(id)) {
        return;
      }
      if (hiddenNativeProfileMenuRows.has(id)) {
        hiddenNativeProfileMenuRows.delete(id);
      } else {
        hiddenNativeProfileMenuRows.add(id);
      }
      writePreferences();
      const identifiedMenu = identifyProfileMenu();
      if (identifiedMenu) {
        applyNativeMenuVisibility(identifiedMenu);
      }
      updateSettingsPage();
      schedule();
      return;
    }
    const maintenanceToggle = target.closest("[data-swapdex-maintenance-toggle]");
    if (maintenanceToggle instanceof HTMLElement) {
      event.preventDefault();
      event.stopPropagation();
      maintenanceEnabled = !maintenanceEnabled;
      writePreferences();
      queueMaintenanceConfig();
      updateSettingsPage();
      return;
    }
    const accountReauth = target.closest("[data-swapdex-account-reauth]");
    if (accountReauth instanceof HTMLButtonElement) {
      event.preventDefault();
      event.stopPropagation();
      const id = accountReauth.dataset.swapdexAccountReauth || "";
      if (!id) {
        return;
      }
      pendingReauths.add(id);
      enqueue({
        v: 1,
        action: "reauth",
        key: id,
        source: "profile-dropdown"
      });
      updateSettingsPage();
      window.setTimeout(() => {
        pendingReauths.delete(id);
        updateSettingsPage();
      }, 120000);
      return;
    }
    const accountWarmup = target.closest("[data-swapdex-account-warmup]");
    if (accountWarmup instanceof HTMLButtonElement) {
      event.preventDefault();
      event.stopPropagation();
      const id = accountWarmup.dataset.swapdexAccountWarmup || "";
      const profile = Array.isArray(snapshot.profiles) ? snapshot.profiles.find(item => item && item.id === id) : null;
      if (!profile || profile.authenticated === false || pendingWarmups.has(id)) {
        return;
      }
      const previous = Number(profile.last_maintenance_at);
      pendingWarmups.set(id, Number.isFinite(previous) ? previous : 0);
      enqueue({
        v: 1,
        action: "maintenance-run-now",
        key: id,
        source: "profile-dropdown"
      });
      updateSettingsPage();
      return;
    }
    const accountRemove = target.closest("[data-swapdex-account-remove]");
    if (accountRemove instanceof HTMLButtonElement) {
      event.preventDefault();
      event.stopPropagation();
      const id = accountRemove.dataset.swapdexAccountRemove || "";
      if (!id || pendingRemovals.has(id)) {
        return;
      }
      const profile = Array.isArray(snapshot.profiles) ? snapshot.profiles.find(item => item && item.id === id) : null;
      if (!profile) {
        return;
      }
      const accountName = normalize(profile.label) || normalize(profile.email) || "This account";
      const isActive = snapshot.active === id;
      openConfirmation({
        title: "Remove account?",
        description: isActive
          ? `${accountName} is signed in. Removing it signs you out and deletes its stored credentials. This cannot be undone.`
          : `Are you sure you want to remove ${accountName}? Its stored credentials will be deleted. This cannot be undone.`,
        confirmLabel: "Remove account",
        cancelLabel: "Cancel",
        onConfirm: () => {
          pendingRemovals.add(id);
          enqueue({
            v: 1,
            action: "remove",
            key: id,
            source: "profile-dropdown"
          });
          updateSettingsPage();
          window.setTimeout(() => {
            pendingRemovals.delete(id);
            updateSettingsPage();
          }, 60000);
        }
      });
      return;
    }
    const runNow = target.closest("[data-swapdex-maintenance-run]");
    if (runNow instanceof HTMLElement) {
      event.preventDefault();
      event.stopPropagation();
      enqueue({
        v: 1,
        action: "maintenance-run-now",
        key: null,
        source: "profile-dropdown"
      });
      return;
    }
    const nativeSidebarButton = target.closest("button.sidebar-item");
    if (nativeSidebarButton && settingsState.active) {
      closeSettingsPage(false);
    }
  }, true);

  document.addEventListener("keydown", event => {
    const target = event.target instanceof Element ? event.target : null;
    if (!target) {
      return;
    }
    const trigger = target.closest("[data-swapdex-maintenance-interval]");
    if (trigger instanceof HTMLButtonElement && (event.key === "Enter" || event.key === " " || event.key === "ArrowDown")) {
      event.preventDefault();
      event.stopPropagation();
      openIntervalMenu(trigger);
      return;
    }
    const option = target.closest("[data-swapdex-interval-option]");
    if (!(option instanceof HTMLButtonElement) || !(intervalMenu instanceof HTMLElement)) {
      return;
    }
    const options = Array.from(intervalMenu.querySelectorAll("[data-swapdex-interval-option]"));
    const index = options.indexOf(option);
    let next = null;
    if (event.key === "ArrowDown") {
      next = options[(index + 1) % options.length];
    } else if (event.key === "ArrowUp") {
      next = options[(index - 1 + options.length) % options.length];
    } else if (event.key === "Home") {
      next = options[0];
    } else if (event.key === "End") {
      next = options.at(-1);
    } else if (event.key === "Escape") {
      event.preventDefault();
      closeIntervalMenu(true);
      return;
    } else if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      event.stopPropagation();
      selectMaintenanceInterval(Number(option.dataset.swapdexIntervalOption));
      return;
    }
    if (next instanceof HTMLElement) {
      event.preventDefault();
      next.focus({ preventScroll: true });
    }
  }, true);

  document.addEventListener("keydown", event => {
    const target = event.target instanceof Element ? event.target.closest("[data-swapdex-settings-tab]") : null;
    if (!(target instanceof HTMLElement) || settingsState.page === null) {
      return;
    }
    const tabs = Array.from(settingsState.page.querySelectorAll("[data-swapdex-settings-tab]"));
    const index = tabs.indexOf(target);
    let next = null;
    if (event.key === "ArrowRight") {
      next = tabs[(index + 1) % tabs.length];
    } else if (event.key === "ArrowLeft") {
      next = tabs[(index - 1 + tabs.length) % tabs.length];
    } else if (event.key === "Home") {
      next = tabs[0];
    } else if (event.key === "End") {
      next = tabs.at(-1);
    }
    if (next instanceof HTMLElement) {
      event.preventDefault();
      activateSettingsTab(next.dataset.swapdexSettingsTab || "privacy");
    }
  }, true);

  const reconcile = () => {
    scheduled = false;
    reconcileSettings();
    markProfileTriggerName();
    const identified = identifyProfileMenu();
    if (!identified) {
      return;
    }
    const knownMenu = menuState.has(identified.menu);
    enhanceMenu(identified);
    if (!knownMenu) {
      request("snapshot");
    }
  };

  const schedule = () => {
    if (scheduled) {
      return;
    }
    scheduled = true;
    requestAnimationFrame(() => requestAnimationFrame(reconcile));
  };

  window.__swapdexApplySnapshot = value => {
    try {
      const parsed = typeof value === "string" ? JSON.parse(value) : value;
      if (!parsed || parsed.v !== 1 || !Array.isArray(parsed.profiles)) {
        return;
      }
      snapshot = {
        active: typeof parsed.active === "string" ? parsed.active : "",
        profiles: parsed.profiles.slice(0, 32).filter(profile => profile && typeof profile.id === "string" && /^[A-Za-z0-9_-]{1,64}$/.test(profile.id))
      };
      renderGeneration += 1;
      schedule();
    } catch {
      return;
    }
  };

  window.__swapdexShowStatus = value => {
    if (typeof value !== "string") {
      return;
    }
    pendingStatus = value.slice(0, 160);
    if (statusTimer !== null) {
      clearTimeout(statusTimer);
    }
    statusTimer = setTimeout(() => {
      pendingStatus = "";
      statusTimer = null;
      schedule();
    }, 5000);
    const identified = identifyProfileMenu();
    if (identified) {
      renderStatus(identified);
    }
  };

  // Painted last, once every declaration in this scope exists. Doing it earlier ran into
  // the temporal dead zone and aborted the whole script, which left the app with nothing
  // injected at all rather than a visible error.
  const observer = new MutationObserver(schedule);
  observer.observe(document.documentElement, { childList: true, subtree: true });
  window.addEventListener("load", schedule, { once: true });
  schedule();
})();
