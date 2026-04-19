<script lang="ts">
  import { invoke } from "@tauri-apps/api/core";
  import { listen } from "@tauri-apps/api/event";
  import { save } from "@tauri-apps/plugin-dialog";
  import { onMount } from "svelte";

  // --- Types ---
  interface DeviceInfo { path: string; version: number }
  interface SectorRow  { sector: number; values: number[] }
  interface ScrubResult { checked: number; healthy: number; repaired: number; unrecoverable: number }
  interface ExtractProgress { current: number; total: number }

  // --- State ---
  let devices:        DeviceInfo[]           = $state([]);
  let selectedDevice: DeviceInfo | null      = $state(null);
  let tableRows:      SectorRow[]            = $state([]);
  let columnNames:    string[]               = $state([]);
  let scanning   = $state(false);
  let extracting = $state(false);
  let extractProgress: ExtractProgress | null = $state(null);
  let scrubResult:    ScrubResult | null     = $state(null);
  let scrubbing  = $state(false);
  let settingsOpen = $state(false);
  let settingsTab: "visual" | "yaml" = $state("visual");
  let yamlText   = $state("");
  let yamlSaving = $state(false);
  let yamlSaveMsg = $state("");
  let extractError = $state("");
  let mirrorCount     = $state(2);
  let sectorSize      = $state(512);
  let metadataSectors = $state(2);

  // SDK Generator state
  let sdkView       = $state(false);
  let sensorName    = $state("my_sensor");
  let busType       = $state("spi");
  let sdkFiles: Record<string, string> = $state({});
  let sdkActiveFile = $state("");
  let sdkGenerating = $state(false);

  onMount(async () => {
    await loadYaml();
    await scanDevices();
    await listen<ExtractProgress>("extract-progress", (e) => {
      extractProgress = e.payload;
    });
  });

  async function loadYaml() {
    try {
      yamlText = await invoke<string>("load_yaml");
    } catch {
      yamlText = "sector_size: 512\nmirror_count: 2\nmetadata_sectors: 2\ndata_types:\n  - name: sensor_t\n    fields:\n      - { name: temp, type: float }\n      - { name: humidity, type: float }\n";
    }
    parseYaml(yamlText);
  }

  function parseYaml(yaml: string) {
    columnNames = [...yaml.matchAll(/name:\s*(\w+)\s*,?\s*type:/g)].map(m => m[1]);
    const mc = yaml.match(/mirror_count:\s*(\d+)/);
    const ss = yaml.match(/sector_size:\s*(\d+)/);
    const ms = yaml.match(/metadata_sectors:\s*(\d+)/);
    if (mc) mirrorCount     = parseInt(mc[1]);
    if (ss) sectorSize      = parseInt(ss[1]);
    if (ms) metadataSectors = parseInt(ms[1]);
  }

  async function scanDevices() {
    scanning = true;
    try { devices = await invoke<DeviceInfo[]>("scan_devices"); }
    catch (e) { console.error(e); }
    finally { scanning = false; }
  }

  function selectDevice(d: DeviceInfo) {
    selectedDevice = d;
    tableRows = []; scrubResult = null; extractProgress = null;
  }

  async function extractData() {
    if (!selectedDevice) return;
    extracting = true; extractProgress = null; tableRows = []; scrubResult = null; extractError = "";
    const csvPath = await save({
      title: "Save CSV", defaultPath: "zinf_data.csv",
      filters: [{ name: "CSV", extensions: ["csv"] }],
    }).catch(() => "");
    try {
      tableRows = await invoke<SectorRow[]>("extract_data", {
        devicePath: selectedDevice.path,
        outputCsv: csvPath ?? "",
        yamlText,
      });
      parseYaml(yamlText);
    } catch (e) {
      extractError = String(e);
      console.error(e);
    }
    finally { extracting = false; }
  }

  async function verifyIntegrity() {
    if (!selectedDevice) return;
    scrubbing = true; scrubResult = null;
    try { scrubResult = await invoke<ScrubResult>("verify_integrity", { devicePath: selectedDevice.path }); }
    catch (e) { console.error(e); }
    finally { scrubbing = false; }
  }

  async function saveAndRegenerate() {
    yamlSaving = true; yamlSaveMsg = "";
    try {
      yamlSaveMsg = await invoke<string>("generate_config", { yamlText });
      parseYaml(yamlText);
    } catch (e) { yamlSaveMsg = String(e); }
    finally { yamlSaving = false; }
  }

  function syncVisualToYaml() {
    yamlText = yamlText
      .replace(/mirror_count:\s*\d+/, `mirror_count: ${mirrorCount}`)
      .replace(/sector_size:\s*\d+/,  `sector_size: ${sectorSize}`)
      .replace(/metadata_sectors:\s*\d+/, `metadata_sectors: ${metadataSectors}`);
  }

  async function generateSdk() {
    sdkGenerating = true; sdkFiles = {};
    try {
      sdkFiles = await invoke<Record<string, string>>("generate_sdk", { sensorName, busType, yamlText });
      sdkActiveFile = Object.keys(sdkFiles)[0] ?? "";
    } catch (e) { console.error(e); }
    finally { sdkGenerating = false; }
  }
</script>

<div class="flex flex-col h-screen bg-gray-950 text-gray-100 font-mono text-sm overflow-hidden select-none">

  <!-- Toolbar -->
  <header class="flex items-center justify-between bg-gray-900 border-b border-gray-700 px-4 py-2 flex-shrink-0">
    <span class="text-amber-400 font-bold tracking-widest">ZINF Studio</span>
    <div class="flex gap-2">
      <button class="px-3 py-1 rounded text-xs transition
        {!sdkView ? 'bg-amber-900/60 text-amber-300' : 'bg-gray-700 hover:bg-gray-600 text-gray-300'}"
        onclick={() => sdkView = false}>Data</button>
      <button class="px-3 py-1 rounded text-xs transition
        {sdkView ? 'bg-amber-900/60 text-amber-300' : 'bg-gray-700 hover:bg-gray-600 text-gray-300'}"
        onclick={() => sdkView = true}>SDK Generator</button>
      <button class="px-3 py-1 rounded text-xs bg-amber-700 hover:bg-amber-600 transition"
        onclick={() => settingsOpen = !settingsOpen}>⚙ Config</button>
    </div>
  </header>

  <div class="flex flex-1 overflow-hidden relative">

    <!-- Left Sidebar -->
    <aside class="w-52 flex-shrink-0 flex flex-col bg-gray-900 border-r border-gray-700 overflow-hidden">
      <div class="px-3 py-2 text-xs text-gray-500 uppercase tracking-wider border-b border-gray-700">Devices</div>
      <div class="flex-1 overflow-y-auto">
        {#each devices as d}
          <button class="w-full text-left px-3 py-2 text-xs border-b border-gray-800 transition
            {selectedDevice?.path === d.path ? 'bg-amber-900/40 text-amber-300' : 'text-gray-300 hover:bg-gray-800'}"
            onclick={() => selectDevice(d)}>
            <div class="font-semibold truncate">{d.path}</div>
            <div class="text-gray-500">zinf v{d.version}</div>
          </button>
        {/each}
        {#if devices.length === 0 && !scanning}
          <div class="px-3 py-4 text-xs text-gray-600 italic">No ZINF devices.<br/>Attach a loop device or SD card.</div>
        {/if}
      </div>
      <div class="px-3 py-2 border-t border-gray-700">
        <button class="w-full py-1 rounded text-xs bg-gray-700 hover:bg-gray-600 transition disabled:opacity-50"
          onclick={scanDevices} disabled={scanning}>
          {scanning ? "Scanning…" : "[ Scan ]"}
        </button>
      </div>
    </aside>

    <!-- Main Panel -->
    <main class="flex-1 flex flex-col overflow-hidden">
      {#if !sdkView}

        {#if !selectedDevice}
          <div class="flex-1 flex items-center justify-center text-gray-600 text-xs">
            Select a device from the sidebar.
          </div>
        {:else}
          <!-- Data table -->
          <div class="flex-1 overflow-auto">
            {#if extracting}
              <div class="flex flex-col items-center justify-center h-full gap-3">
                <div class="text-amber-400 text-xs animate-pulse">Extracting…</div>
                {#if extractProgress}
                  <div class="w-64 bg-gray-700 rounded-full h-1.5">
                    <div class="bg-amber-500 h-1.5 rounded-full transition-all"
                      style="width:{Math.round((extractProgress.current/extractProgress.total)*100)}%"></div>
                  </div>
                  <div class="text-gray-500 text-xs">{extractProgress.current} / {extractProgress.total} sectors</div>
                {/if}
              </div>
            {:else if tableRows.length > 0}
              <table class="w-full text-xs border-collapse">
                <thead>
                  <tr class="bg-gray-800 sticky top-0 z-10">
                    <th class="px-3 py-2 text-left text-gray-400 border-b border-gray-700">sector</th>
                    {#each columnNames as col}
                      <th class="px-3 py-2 text-left text-gray-400 border-b border-gray-700">{col}</th>
                    {/each}
                  </tr>
                </thead>
                <tbody>
                  {#each tableRows as row, i}
                    <tr class="border-b border-gray-800 hover:bg-gray-800 {i%2===0?'bg-gray-900':'bg-gray-900/60'}">
                      <td class="px-3 py-1 text-gray-500">{row.sector}</td>
                      {#each row.values as v}
                        <td class="px-3 py-1 text-amber-300">{v.toFixed(4)}</td>
                      {/each}
                    </tr>
                  {/each}
                </tbody>
              </table>
            {:else if extractError}
              <div class="flex items-center justify-center h-full">
                <div class="text-red-400 text-xs max-w-md text-center break-words px-4">
                  <div class="font-semibold mb-1">Extraction failed</div>
                  {extractError}
                </div>
              </div>
            {:else}
              <div class="flex items-center justify-center h-full text-gray-600 text-xs">
                Click [Extract to CSV] to load data.
              </div>
            {/if}
          </div>

          <!-- Action bar -->
          <div class="flex items-center gap-3 px-4 py-2.5 border-t border-gray-700 bg-gray-900 flex-shrink-0">
            {#if scrubbing}
              <span class="text-xs text-amber-400 animate-pulse">Verifying integrity…</span>
            {:else if scrubResult}
              <span class="text-xs text-green-400">
                ✓ {scrubResult.checked} checked · {scrubResult.repaired} repaired · {scrubResult.unrecoverable} unrecoverable
              </span>
            {/if}
            <div class="ml-auto flex gap-2">
              <button class="px-4 py-1.5 rounded text-xs bg-amber-700 hover:bg-amber-600 transition disabled:opacity-50"
                onclick={extractData} disabled={extracting}>
                {extracting ? "Extracting…" : "[ Extract to CSV ]"}
              </button>
              <button class="px-4 py-1.5 rounded text-xs bg-gray-700 hover:bg-gray-600 transition disabled:opacity-50"
                onclick={verifyIntegrity} disabled={scrubbing}>
                {scrubbing ? "Verifying…" : "[ Verify Integrity ]"}
              </button>
            </div>
          </div>
        {/if}

      {:else}
        <!-- SDK Generator view -->
        <div class="flex-1 overflow-auto p-5">
          <div class="max-w-3xl mx-auto">
            <h2 class="text-amber-400 font-bold mb-4 tracking-wider text-xs uppercase">Guided SDK Generator</h2>
            <div class="flex flex-wrap gap-3 mb-5">
              <div class="flex flex-col gap-1">
                <label class="text-xs text-gray-400">Sensor name</label>
                <input class="bg-gray-800 border border-gray-600 rounded px-2 py-1 text-xs text-gray-100 w-40"
                  bind:value={sensorName} placeholder="e.g. bme280" />
              </div>
              <div class="flex flex-col gap-1">
                <label class="text-xs text-gray-400">Bus type</label>
                <select class="bg-gray-800 border border-gray-600 rounded px-2 py-1 text-xs text-gray-100"
                  bind:value={busType}>
                  <option value="spi">SPI</option>
                  <option value="i2c">I2C</option>
                  <option value="uart">UART</option>
                  <option value="custom">Custom</option>
                </select>
              </div>
              <div class="flex items-end">
                <button class="px-4 py-1.5 rounded text-xs bg-amber-700 hover:bg-amber-600 transition disabled:opacity-50"
                  onclick={generateSdk} disabled={sdkGenerating}>
                  {sdkGenerating ? "Generating…" : "[ Generate SDK ]"}
                </button>
              </div>
            </div>

            {#if Object.keys(sdkFiles).length > 0}
              <div class="flex gap-0.5 mb-0 flex-wrap">
                {#each Object.keys(sdkFiles) as fname}
                  <button class="px-3 py-1 text-xs rounded-t border-x border-t transition
                    {sdkActiveFile===fname
                      ? 'bg-gray-800 border-gray-600 text-amber-300'
                      : 'bg-gray-900 border-gray-700 text-gray-500 hover:text-gray-300'}"
                    onclick={() => sdkActiveFile = fname}>{fname}</button>
                {/each}
              </div>
              <div class="bg-gray-800 border border-gray-600 rounded-b rounded-tr p-4 overflow-x-auto max-h-[60vh]">
                <pre class="text-xs text-green-300 whitespace-pre">{sdkFiles[sdkActiveFile] ?? ""}</pre>
              </div>
            {/if}
          </div>
        </div>
      {/if}
    </main>

    <!-- Settings Drawer -->
    {#if settingsOpen}
      <aside class="w-80 flex-shrink-0 flex flex-col bg-gray-900 border-l border-gray-700 overflow-hidden">
        <div class="flex items-center justify-between px-4 py-2 border-b border-gray-700 flex-shrink-0">
          <span class="text-xs text-gray-400 uppercase tracking-wider">Configuration</span>
          <button class="text-gray-500 hover:text-gray-200 text-xs" onclick={() => settingsOpen = false}>✕</button>
        </div>
        <div class="flex border-b border-gray-700 flex-shrink-0">
          <button class="flex-1 py-2 text-xs transition
            {settingsTab==='visual' ? 'text-amber-400 border-b-2 border-amber-400' : 'text-gray-400 hover:text-gray-200'}"
            onclick={() => settingsTab='visual'}>Visual</button>
          <button class="flex-1 py-2 text-xs transition
            {settingsTab==='yaml' ? 'text-amber-400 border-b-2 border-amber-400' : 'text-gray-400 hover:text-gray-200'}"
            onclick={() => settingsTab='yaml'}>YAML</button>
        </div>
        <div class="flex-1 overflow-y-auto p-4">
          {#if settingsTab === 'visual'}
            <div class="flex flex-col gap-5">
              <label class="flex flex-col gap-1">
                <span class="text-xs text-gray-400">mirror_count: <strong class="text-gray-200">{mirrorCount}</strong></span>
                <input type="range" min="1" max="5" class="accent-amber-500"
                  bind:value={mirrorCount} oninput={syncVisualToYaml} />
              </label>
              <label class="flex flex-col gap-1">
                <span class="text-xs text-gray-400">sector_size</span>
                <select class="bg-gray-800 border border-gray-600 rounded px-2 py-1 text-xs text-gray-100"
                  bind:value={sectorSize} onchange={syncVisualToYaml}>
                  <option value={512}>512</option>
                  <option value={1024}>1024</option>
                  <option value={4096}>4096</option>
                </select>
              </label>
              <label class="flex flex-col gap-1">
                <span class="text-xs text-gray-400">metadata_sectors: <strong class="text-gray-200">{metadataSectors}</strong></span>
                <input type="range" min="1" max="8" class="accent-amber-500"
                  bind:value={metadataSectors} oninput={syncVisualToYaml} />
              </label>
            </div>
          {:else}
            <textarea class="w-full h-64 bg-gray-800 border border-gray-600 rounded p-2
              text-xs text-green-300 font-mono resize-none focus:outline-none focus:border-amber-600"
              bind:value={yamlText}></textarea>
          {/if}
        </div>
        <div class="px-4 py-3 border-t border-gray-700 flex-shrink-0">
          {#if yamlSaveMsg}
            <div class="text-xs mb-2 {yamlSaveMsg.toLowerCase().includes('success') ? 'text-green-400' : 'text-red-400'}">
              {yamlSaveMsg}
            </div>
          {/if}
          <button class="w-full py-2 rounded text-xs bg-amber-700 hover:bg-amber-600 transition disabled:opacity-50"
            onclick={saveAndRegenerate} disabled={yamlSaving}>
            {yamlSaving ? "Regenerating…" : "[ Save & Regenerate ]"}
          </button>
        </div>
      </aside>
    {/if}

  </div>
</div>
