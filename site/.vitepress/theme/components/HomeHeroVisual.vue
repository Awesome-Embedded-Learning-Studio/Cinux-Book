<template>
  <div class="boot-board" aria-label="Cinux 从固件启动到桌面的内核启动轨迹">
    <div class="boot-board__grid" aria-hidden="true" />

    <header class="boot-board__head">
      <div class="boot-board__brand">
        <span class="boot-board__mark">CX</span>
        <span>CINUX // BOOT TRACE</span>
      </div>
      <span class="boot-board__live"><i /> LIVE</span>
    </header>

    <div class="boot-board__core">
      <div class="boot-board__rings" aria-hidden="true">
        <span class="ring ring--1" />
        <span class="ring ring--2" />
        <span class="ring ring--3" />
        <strong>64</strong>
        <small>BIT</small>
      </div>

      <ol class="boot-board__trace">
        <li class="is-done">
          <span class="trace-id">00</span>
          <span><b>BIOS</b><small>boot sector · 0x7C00</small></span>
          <em>OK</em>
        </li>
        <li class="is-done">
          <span class="trace-id">01</span>
          <span><b>LONG MODE</b><small>GDT · paging · x86_64</small></span>
          <em>OK</em>
        </li>
        <li class="is-active">
          <span class="trace-id">02</span>
          <span><b>kernel_main()</b><small>memory · process · VFS</small></span>
          <em>RUN</em>
        </li>
        <li>
          <span class="trace-id">03</span>
          <span><b>USERLAND</b><small>shell · GUI · network</small></span>
          <em>NEXT</em>
        </li>
      </ol>
    </div>

    <footer class="boot-board__foot">
      <span>target: x86_64</span>
      <span>lang: C++17</span>
      <span>machine: QEMU</span>
    </footer>
  </div>
</template>

<style scoped>
.boot-board {
  position: relative;
  box-sizing: border-box;
  width: min(100%, 476px);
  min-width: 0;
  height: 316px;
  margin: 0 auto;
  overflow: hidden;
  color: #d9fff6;
  background:
    radial-gradient(circle at 18% 18%, rgba(48, 211, 178, 0.16), transparent 34%),
    linear-gradient(145deg, #071713, #0b2a23 68%, #0b342b);
  border: 1px solid rgba(93, 233, 202, 0.28);
  border-radius: 22px 8px 22px 8px;
  box-shadow: 0 28px 70px rgba(2, 31, 25, 0.28), inset 0 0 0 1px rgba(255, 255, 255, 0.025);
  isolation: isolate;
}

.boot-board__grid {
  position: absolute;
  inset: 0;
  z-index: -1;
  opacity: 0.2;
  background-image:
    linear-gradient(rgba(101, 240, 211, 0.16) 1px, transparent 1px),
    linear-gradient(90deg, rgba(101, 240, 211, 0.16) 1px, transparent 1px);
  background-size: 22px 22px;
  mask-image: linear-gradient(to bottom, #000, transparent 88%);
}

.boot-board__head,
.boot-board__foot {
  display: flex;
  align-items: center;
  justify-content: space-between;
  font-family: var(--vp-font-family-mono);
  letter-spacing: 0.08em;
}

.boot-board__head {
  height: 48px;
  padding: 0 18px;
  border-bottom: 1px solid rgba(111, 230, 205, 0.13);
  font-size: 11px;
}

.boot-board__brand { display: flex; align-items: center; gap: 9px; color: #9ac8bc; }
.boot-board__mark {
  display: grid;
  width: 24px;
  height: 24px;
  place-items: center;
  color: #071713;
  background: #49d9bb;
  border-radius: 7px 2px 7px 2px;
  font-size: 10px;
  font-weight: 900;
}
.boot-board__live { display: inline-flex; align-items: center; gap: 7px; color: #66e3c7; }
.boot-board__live i {
  width: 7px;
  height: 7px;
  background: currentColor;
  border-radius: 50%;
  box-shadow: 0 0 0 5px rgba(102, 227, 199, 0.1);
  animation: boot-pulse 1.8s ease-in-out infinite;
}

.boot-board__core {
  display: grid;
  grid-template-columns: 108px minmax(0, 1fr);
  align-items: center;
  gap: 18px;
  height: 220px;
  padding: 14px 18px;
}

.boot-board__rings {
  position: relative;
  display: grid;
  width: 104px;
  height: 104px;
  place-content: center;
  text-align: center;
}
.ring { position: absolute; border: 1px solid rgba(86, 224, 195, 0.28); border-radius: 50%; }
.ring--1 { inset: 0; border-style: dashed; animation: boot-spin 18s linear infinite; }
.ring--2 { inset: 13px; border-color: rgba(86, 224, 195, 0.46); }
.ring--3 { inset: 25px; background: rgba(55, 202, 170, 0.08); border-color: #35c8aa; box-shadow: 0 0 32px rgba(53, 200, 170, 0.15); }
.boot-board__rings strong { z-index: 1; font: 800 27px/1 var(--vp-font-family-mono); color: #f3fffc; }
.boot-board__rings small { z-index: 1; margin-top: 4px; font: 700 9px/1 var(--vp-font-family-mono); letter-spacing: 0.18em; color: #57d7bb; }

.boot-board__trace { min-width: 0; margin: 0; padding: 0; list-style: none; }
.boot-board__trace li {
  display: grid;
  grid-template-columns: 25px minmax(0, 1fr) auto;
  align-items: center;
  gap: 10px;
  min-height: 44px;
  padding: 4px 0;
  color: #78958e;
  border-bottom: 1px solid rgba(129, 211, 194, 0.09);
}
.boot-board__trace li:last-child { border-bottom: 0; }
.trace-id { font: 600 10px/1 var(--vp-font-family-mono); color: #4c7269; }
.boot-board__trace b,
.boot-board__trace small { display: block; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.boot-board__trace b { font: 700 11px/1.35 var(--vp-font-family-mono); letter-spacing: 0.04em; }
.boot-board__trace small { margin-top: 2px; color: #67877f; font: 10px/1.25 var(--vp-font-family-mono); }
.boot-board__trace em { font: 700 9px/1 var(--vp-font-family-mono); font-style: normal; letter-spacing: 0.08em; }
.boot-board__trace .is-done { color: #b5d4cc; }
.boot-board__trace .is-done em { color: #4dd7b9; }
.boot-board__trace .is-active { color: #f2fffc; }
.boot-board__trace .is-active em { padding: 5px 7px; color: #06221c; background: #54dfc0; border-radius: 4px; animation: boot-glow 1.8s ease-in-out infinite; }

.boot-board__foot {
  height: 48px;
  padding: 0 18px;
  color: #61867d;
  border-top: 1px solid rgba(111, 230, 205, 0.12);
  background: rgba(0, 0, 0, 0.12);
  font-size: 9px;
}

@keyframes boot-spin { to { transform: rotate(360deg); } }
@keyframes boot-pulse { 50% { opacity: 0.45; transform: scale(0.75); } }
@keyframes boot-glow { 50% { box-shadow: 0 0 18px rgba(84, 223, 192, 0.35); } }

@media (max-width: 479px) {
  .boot-board { height: 292px; border-radius: 18px 6px 18px 6px; }
  .boot-board__core { grid-template-columns: 76px minmax(0, 1fr); gap: 12px; height: 198px; padding: 10px 12px; }
  .boot-board__rings { width: 74px; height: 74px; }
  .ring--2 { inset: 9px; }
  .ring--3 { inset: 18px; }
  .boot-board__rings strong { font-size: 21px; }
  .boot-board__head, .boot-board__foot { height: 47px; padding: 0 12px; }
  .boot-board__foot span:nth-child(2) { display: none; }
}

@media (prefers-reduced-motion: reduce) {
  .ring--1, .boot-board__live i, .boot-board__trace .is-active em { animation: none; }
}
</style>
