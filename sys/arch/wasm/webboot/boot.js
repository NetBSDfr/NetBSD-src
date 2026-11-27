// NetBSD/wasm browser host - UI thread.
//
// Boots the kernel automatically and relays its console to <pre>.
// The kernel itself runs in kernel_worker.js: its entry point never
// returns (the scheduler spins forever), so it cannot run on this
// thread without freezing the page.  Nothing here depends on the
// NetBSD source tree; see index.html for the two files to place
// alongside this one.
"use strict";

const pre = document.getElementById("console");
const status = document.getElementById("status");
const decoder = new TextDecoder();

let worker = null;

function boot() {
  if (worker)
    worker.terminate();
  pre.textContent = "";
  status.textContent = "booting...";
  status.style.color = "#7a7";

  worker = new Worker("kernel_worker.js");
  worker.onmessage = ({ data }) => {
    if (data.out !== undefined) {
      pre.textContent += decoder.decode(data.out, { stream: true });
      pre.scrollTop = pre.scrollHeight;
    } else if (data.done !== undefined) {
      status.textContent = data.done;
      status.style.color = data.fatal ? "#c66" : "#7a7";
    }
  };
  worker.onerror = (e) => {
    status.textContent = "host error";
    status.style.color = "#c66";
    pre.textContent += `\n[host] ${e.message}\n`;
  };

  // console input: forward printable keys to the kernel's input ring
  document.onkeydown = (e) => {
    if (!worker) return;
    let c = -1;
    if (e.key === "Enter") c = 13;
    else if (e.key === "Backspace") c = 127;
    else if (e.key.length === 1 && e.key.charCodeAt(0) >= 32
             && e.key.charCodeAt(0) < 127)
      c = e.key.charCodeAt(0);
    if (c < 0) return;
    e.preventDefault();
    worker.postMessage({ in: new Uint8Array([c]) });
  };
}

document.getElementById("boot").addEventListener("click", boot);
boot();
