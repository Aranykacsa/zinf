<script lang="ts">
  import { invoke } from "@tauri-apps/api/core";
  import { listen } from "@tauri-apps/api/event";
  import { save, open } from "@tauri-apps/plugin-dialog";
  import { readTextFile, writeTextFile } from "@tauri-apps/plugin-fs";
  import { onMount } from "svelte";
  import { 
    Cpu, 
    Database, 
    Settings, 
    HardDrive, 
    Search, 
    Activity, 
    Download, 
    Code, 
    FileJson, 
    CheckCircle2, 
    AlertCircle,
    ChevronRight,
    RefreshCw,
    Plus,
    Trash2,
    FileUp,
    FileDown
  } from "lucide-svelte";
  import { cn } from "$lib/utils";

  // --- Types ---
  interface DeviceInfo { path: string; version: number }
  interface SectorRow  { sector: number; index: number; values: number[] }
  interface ScrubResult { checked: number; healthy: number; repaired: number; unrecoverable: number }
  interface ExtractProgress { current: number; total: number }

  interface YamlField { name: string; type: string }
  interface YamlDataType { name: string; fields: YamlField[] }
  interface ZinfYamlConfig {
    sector_size: number;
    mirror_count: number;
    metadata_sectors: number;
    header_size: number;
    max_bad_sectors: number;
    data_types: YamlDataType[];
  }

  // --- State ---
  let activeTab: "explorer" | "configurator" | "sdk" = $state("explorer");
  let devices:        DeviceInfo[]           = $state([]);
  let selectedDevice: DeviceInfo | null      = $state(null);
  let tableRows:      SectorRow[]            = $state([]);
  let scanning   = $state(false);
  let extracting = $state(false);
  let extractProgress: ExtractProgress | null = $state(null);
  let scrubResult:    ScrubResult | null     = $state(null);
  let scrubbing  = $state(false);
  
  let previewMode = $state(false);
  let previewFilename = $state("");
  let columnNamesFromCsv: string[] = $state([]);

  let settingsTab: "visual" | "yaml" = $state("visual");
  let yamlText   = $state("");
  let yamlSaving = $state(false);
  let yamlSaveMsg = $state("");
  let extractError = $state("");

  let config: ZinfYamlConfig = $state({
    sector_size: 512,
    mirror_count: 2,
    metadata_sectors: 2,
    header_size: 1,
    max_bad_sectors: 64,
    data_types: [
      { name: "sensor_t", fields: [
        { name: "temp", type: "float" },
        { name: "humidity", type: "float" }
      ]}
    ]
  });

  // Derived columns for the explorer table
  let columnNames = $derived(previewMode ? columnNamesFromCsv : (config.data_types[0]?.fields.map(f => f.name) ?? []));

  // SDK Generator state
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
      await syncYamlToVisual();
    } catch {
      yamlText = "sector_size: 512\nmirror_count: 2\nmetadata_sectors: 2\nheader_size: 1\nmax_bad_sectors: 64\ndata_types:\n  - name: sensor_t\n    fields:\n      - { name: temp, type: float }\n      - { name: humidity, type: float }\n";
      await syncYamlToVisual();
    }
  }

  async function syncYamlToVisual() {
    try {
      config = await invoke<ZinfYamlConfig>("get_config", { yamlText });
    } catch (e) {
      console.error("Visual sync error:", e);
    }
  }

  async function syncVisualToYaml() {
    try {
      yamlText = await invoke<string>("serialize_config", { config: $state.snapshot(config) });
    } catch (e) {
      console.error("YAML sync error:", e);
    }
  }

  async function importYaml() {
    const path = await open({
      multiple: false,
      filters: [{ name: "YAML", extensions: ["yaml", "yml"] }]
    });
    if (path) {
      yamlText = await readTextFile(path);
      await syncYamlToVisual();
    }
  }

  async function exportYaml() {
    const path = await save({
      title: "Export YAML",
      defaultPath: "zinf_exported.yaml",
      filters: [{ name: "YAML", extensions: ["yaml", "yml"] }]
    });
    if (path) {
      await syncVisualToYaml();
      await writeTextFile(path, yamlText);
    }
  }

  async function scanDevices() {
    scanning = true;
    try { devices = await invoke<DeviceInfo[]>("scan_devices"); }
    catch (e) { console.error(e); }
    finally { scanning = false; }
  }

  function selectDevice(d: DeviceInfo) {
    selectedDevice = d;
    previewMode = false;
    previewFilename = "";
    tableRows = []; scrubResult = null; extractProgress = null;
  }

  async function openLocalCsv() {
    const path = await open({
      multiple: false,
      filters: [{ name: "CSV", extensions: ["csv"] }]
    });
    if (path) {
      try {
        const [headers, rows] = await invoke<[string[], SectorRow[]]>("preview_csv", { path });
        selectedDevice = null;
        previewMode = true;
        previewFilename = typeof path === 'string' ? path.split('/').pop() ?? "unnamed.csv" : "unnamed.csv";
        columnNamesFromCsv = headers;
        tableRows = rows;
      } catch (e) {
        extractError = String(e);
      }
    }
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
      await syncYamlToVisual();
    } catch (e) { yamlSaveMsg = String(e); }
    finally { yamlSaving = false; }
  }

  async function generateSdk() {
    sdkGenerating = true; sdkFiles = {};
    try {
      sdkFiles = await invoke<Record<string, string>>("generate_sdk", { sensorName, busType, yamlText });
      sdkActiveFile = Object.keys(sdkFiles)[0] ?? "";
    } catch (e) { console.error(e); }
    finally { sdkGenerating = false; }
  }

  // --- Visual Config Helpers ---
  function addDataType() {
    config.data_types = [...config.data_types, { name: "new_type_t", fields: [] }];
    syncVisualToYaml();
  }

  function removeDataType(index: number) {
    config.data_types = config.data_types.filter((_, i) => i !== index);
    syncVisualToYaml();
  }

  function addField(dtIndex: number) {
    config.data_types[dtIndex].fields = [...config.data_types[dtIndex].fields, { name: "new_field", type: "float" }];
    syncVisualToYaml();
  }

  function removeField(dtIndex: number, fIndex: number) {
    config.data_types[dtIndex].fields = config.data_types[dtIndex].fields.filter((_, i) => i !== fIndex);
    syncVisualToYaml();
  }
</script>

<!-- HeaderBar (GNOME HIG inspired + 90s Retro) -->
<header class="h-14 border-b bg-secondary flex items-center px-4 shrink-0 z-20 shadow-sm">
  <div class="flex items-center gap-2 mr-8">
    <div class="p-1.5 bg-primary rounded-lg text-primary-foreground">
      <Cpu size={18} />
    </div>
    <span class="font-black tracking-tighter text-base uppercase">ZINF<span class="font-normal opacity-60">Studio</span></span>
  </div>

  <!-- Segmented Control Tabs (Classic Switch look) -->
  <div class="flex-1 flex justify-center">
    <div class="bg-background/50 p-1 rounded-xl flex gap-1 border border-border shadow-inner">
      <button 
        onclick={() => activeTab = "explorer"}
        class={cn(
          "px-5 py-1.5 rounded-lg text-[11px] font-bold uppercase tracking-wider transition-all flex items-center gap-2",
          activeTab === "explorer" ? "bg-primary text-primary-foreground shadow-sm" : "text-muted-foreground hover:text-foreground hover:bg-muted"
        )}>
        <Database size={13} /> Explorer
      </button>
      <button 
        onclick={() => activeTab = "configurator"}
        class={cn(
          "px-5 py-1.5 rounded-lg text-[11px] font-bold uppercase tracking-wider transition-all flex items-center gap-2",
          activeTab === "configurator" ? "bg-primary text-primary-foreground shadow-sm" : "text-muted-foreground hover:text-foreground hover:bg-muted"
        )}>
        <Settings size={13} /> Configurator
      </button>
      <button 
        onclick={() => activeTab = "sdk"}
        class={cn(
          "px-5 py-1.5 rounded-lg text-[11px] font-bold uppercase tracking-wider transition-all flex items-center gap-2",
          activeTab === "sdk" ? "bg-primary text-primary-foreground shadow-sm" : "text-muted-foreground hover:text-foreground hover:bg-muted"
        )}>
        <Code size={13} /> SDK Gen
      </button>
    </div>
  </div>

  <div class="w-40 flex justify-end">
    <div class="text-[9px] font-bold text-muted-foreground bg-muted px-2 py-1 rounded border border-border">v4.0.0-PROTOTYPE</div>
  </div>
</header>

<div class="flex-1 flex overflow-hidden">
  
  <!-- Sidebar -->
  <aside class="w-64 bg-secondary/40 border-r flex flex-col shrink-0">
    <div class="p-4 flex items-center justify-between border-b border-border/40">
      <h3 class="text-[10px] font-black text-muted-foreground uppercase tracking-widest flex items-center gap-2">
        <HardDrive size={12} /> Hardware List
      </h3>
      <button 
        onclick={scanDevices}
        class="p-1.5 rounded-md hover:bg-muted text-muted-foreground hover:text-primary transition-colors disabled:opacity-50 border border-transparent hover:border-border shadow-sm active:shadow-inner"
        title="Scan Devices"
        disabled={scanning}>
        <RefreshCw size={14} class={scanning ? "animate-spin" : ""} />
      </button>
    </div>

    <div class="flex-1 overflow-y-auto p-2 space-y-1">
      {#each devices as d}
        <button 
          onclick={() => selectDevice(d)}
          class={cn(
            "w-full text-left px-3 py-3 rounded-xl transition-all group relative overflow-hidden border",
            selectedDevice?.path === d.path 
              ? "bg-background border-border shadow-sm text-foreground" 
              : "bg-transparent border-transparent text-muted-foreground hover:bg-muted/60"
          )}>
          <div class="flex items-center justify-between mb-1">
            <span class="text-xs font-bold truncate tracking-tight">{d.path.split('/').pop()}</span>
            <span class="text-[9px] font-bold opacity-60">V{d.version}</span>
          </div>
          <div class="text-[10px] opacity-40 font-mono truncate">{d.path}</div>
          {#if selectedDevice?.path === d.path}
            <div class="absolute right-3 top-1/2 -translate-y-1/2 text-primary">
              <ChevronRight size={16} />
            </div>
          {/if}
        </button>
      {/each}

      {#if devices.length === 0 && !scanning}
        <div class="flex flex-col items-center justify-center p-12 text-center opacity-30 space-y-3">
          <Search size={32} />
          <p class="text-[10px] uppercase font-black tracking-widest leading-tight">No Devices<br/>Detected</p>
        </div>
      {/if}
    </div>
  </aside>

  <!-- Content Area -->
  <main class="flex-1 bg-background p-6 overflow-hidden flex flex-col">
    <div class="flex-1 bg-secondary/20 rounded-2xl border-2 border-border/50 flex flex-col overflow-hidden shadow-sm relative">
      
      {#if activeTab === "explorer"}
        {@render ExplorerView()}
      {:else if activeTab === "configurator"}
        {@render ConfiguratorView()}
      {:else}
        {@render SdkView()}
      {/if}

    </div>
  </main>
</div>

<!-- --- Snippets --- -->

{#snippet ExplorerView()}
  <div class="flex-1 flex flex-col overflow-hidden">
    {#if !selectedDevice && !previewMode}
      <div class="flex-1 flex flex-col items-center justify-center opacity-20 gap-6">
        <Database size={64} strokeWidth={1} />
        <div class="text-center space-y-4">
          <p class="text-[11px] font-bold uppercase tracking-widest leading-relaxed">Awaiting Device Selection<br/>or Offline Payload</p>
          <button 
            onclick={openLocalCsv}
            class="px-6 py-2 rounded-xl border-2 border-primary/30 text-primary hover:bg-primary hover:text-primary-foreground text-[10px] font-black uppercase tracking-widest transition-all shadow-sm">
            [ Load Offline CSV ]
          </button>
        </div>
      </div>
    {:else}
      <div class="flex-1 overflow-auto">
        {#if extracting}
          <div class="flex flex-col items-center justify-center h-full gap-8">
            <div class="relative">
              <div class="w-20 h-20 rounded-full border-[6px] border-primary/10 border-t-primary animate-spin"></div>
              <Database class="absolute top-1/2 left-1/2 -translate-x-1/2 -translate-y-1/2 text-primary" size={28} />
            </div>
            <div class="text-center space-y-3">
              <div class="text-xs font-black text-primary uppercase tracking-[0.2em] animate-pulse">Streaming Data</div>
              {#if extractProgress}
                <div class="w-72 bg-muted rounded-full h-2 border border-border overflow-hidden shadow-inner">
                  <div class="bg-primary h-full transition-all duration-300"
                    style="width:{Math.round((extractProgress.current/extractProgress.total)*100)}%"></div>
                </div>
                <div class="text-[10px] text-muted-foreground font-bold font-mono tracking-tighter">{extractProgress.current} / {extractProgress.total} SECTORS</div>
              {/if}
            </div>
          </div>
        {:else if tableRows.length > 0}
          <div class="p-6">
            <table class="w-full border-collapse">
              <thead>
                <tr class="text-left text-[10px] font-black text-primary uppercase tracking-widest bg-secondary/60 sticky top-0 z-10">
                  <th class="px-4 py-4 border-b-2 border-primary/20">Sector</th>
                  <th class="px-4 py-4 border-b-2 border-primary/20">Index</th>
                  {#each columnNames as col}
                    <th class="px-4 py-4 border-b-2 border-primary/20">{col}</th>
                  {/each}
                </tr>
              </thead>
              <tbody class="text-[11px] font-medium font-mono text-foreground/80">
                {#each tableRows as row, i}
                  <tr class="group hover:bg-background transition-colors border-b border-border/40 {i%2===0?'bg-muted/30':'bg-transparent'}">
                    <td class="px-4 py-2 font-black text-muted-foreground/60">{row.sector}</td>
                    <td class="px-4 py-2 text-muted-foreground/40 italic">{row.index}</td>
                    {#each row.values as v}
                      <td class="px-4 py-2 font-bold text-primary">{v.toFixed(4)}</td>
                    {/each}
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
        {:else if extractError}
          <div class="flex-1 flex flex-col items-center justify-center p-12 text-center gap-4">
            <div class="p-5 bg-destructive/10 text-destructive rounded-full border-2 border-destructive/20">
              <AlertCircle size={40} />
            </div>
            <div class="space-y-1">
              <h4 class="font-black text-foreground uppercase tracking-widest">Hardware I/O Error</h4>
              <p class="text-[11px] text-muted-foreground max-w-sm leading-relaxed">{extractError}</p>
            </div>
          </div>
        {:else}
          <div class="flex-1 flex flex-col items-center justify-center opacity-20 gap-4">
            <Database size={64} strokeWidth={1} />
            <p class="text-[11px] font-bold uppercase tracking-widest">Execute [Extract to CSV] to view data</p>
          </div>
        {/if}
      </div>

      <!-- Footer Bar -->
      <footer class="h-16 border-t border-border/60 bg-secondary/40 px-6 flex items-center shrink-0">
        {#if scrubbing}
          <div class="flex items-center gap-3 text-accent animate-pulse">
            <Activity size={16} />
            <span class="text-[10px] font-black uppercase tracking-widest">Verifying Storage Consistency…</span>
          </div>
        {:else if previewMode}
          <div class="flex items-center gap-3 text-primary">
            <FileJson size={16} />
            <span class="text-[10px] font-black uppercase tracking-widest">OFFLINE PREVIEW: <span class="opacity-60">{previewFilename}</span></span>
          </div>
        {:else if scrubResult}
          <div class="flex items-center gap-6 text-[10px] font-black uppercase tracking-tight">
            <div class="flex items-center gap-2 text-accent">
              <CheckCircle2 size={14} />
              <span>{scrubResult.healthy} <span class="opacity-50">HEALTHY</span></span>
            </div>
            <div class="flex items-center gap-2 text-primary">
              <Activity size={14} />
              <span>{scrubResult.repaired} <span class="opacity-50">REPAIRED</span></span>
            </div>
            <div class="flex items-center gap-2 text-destructive">
              <AlertCircle size={14} />
              <span>{scrubResult.unrecoverable} <span class="opacity-50">FAILED</span></span>
            </div>
          </div>
        {/if}

        <div class="ml-auto flex gap-4">
          <button 
            onclick={verifyIntegrity}
            disabled={scrubbing || extracting || previewMode}
            class="px-5 py-1.5 rounded-xl border-2 border-border bg-background hover:bg-muted text-[10px] font-black uppercase tracking-widest transition-all disabled:opacity-30 active:translate-y-0.5">
            [ Verify Integrity ]
          </button>
          <button 
            onclick={extractData}
            disabled={extracting || previewMode}
            class="px-6 py-1.5 rounded-xl bg-primary text-primary-foreground hover:bg-foreground transition-all shadow-md active:translate-y-0.5 text-[10px] font-black uppercase tracking-widest flex items-center gap-2">
            <Download size={15} /> [ Extract Data ]
          </button>
        </div>
      </footer>
    {/if}
  </div>
{/snippet}

{#snippet ConfiguratorView()}
  <div class="flex-1 flex flex-col overflow-hidden">
    <div class="flex items-center justify-between border-b border-border/40 bg-secondary/40 px-4 py-1">
      <div class="flex gap-1">
        <button 
          onclick={() => settingsTab = "visual"}
          class={cn(
            "px-6 py-2 text-[10px] font-black uppercase tracking-widest transition-all rounded-xl",
            settingsTab === "visual" ? "bg-primary text-primary-foreground shadow-sm" : "text-muted-foreground hover:bg-muted"
          )}>
          Visual Config
        </button>
        <button 
          onclick={() => settingsTab = "yaml"}
          class={cn(
            "px-6 py-2 text-[10px] font-black uppercase tracking-widest transition-all rounded-xl",
            settingsTab === "yaml" ? "bg-primary text-primary-foreground shadow-sm" : "text-muted-foreground hover:bg-muted"
          )}>
          Raw YAML (v4)
        </button>
      </div>
      
      <div class="flex gap-2">
        <button 
          onclick={importYaml}
          class="p-2 rounded-xl hover:bg-muted text-muted-foreground hover:text-primary transition-all shadow-sm active:shadow-inner border border-transparent hover:border-border"
          title="Import YAML File">
          <FileUp size={16} />
        </button>
        <button 
          onclick={exportYaml}
          class="p-2 rounded-xl hover:bg-muted text-muted-foreground hover:text-primary transition-all shadow-sm active:shadow-inner border border-transparent hover:border-border"
          title="Export YAML File">
          <FileDown size={16} />
        </button>
      </div>
    </div>

    <div class="flex-1 overflow-y-auto p-10">
      {#if settingsTab === "visual"}
        <div class="max-w-4xl mx-auto space-y-12">
          
          <div class="grid grid-cols-1 md:grid-cols-2 gap-12">
            <!-- Left Column: Global Settings -->
            <div class="space-y-10">
              <h3 class="text-xs font-black uppercase tracking-[0.2em] text-primary border-b border-primary/10 pb-2">Global Constants</h3>
              
              <!-- Mirror Count -->
              <div class="space-y-4">
                <div class="flex justify-between items-end">
                  <div>
                    <h4 class="text-[10px] font-black uppercase tracking-widest mb-0.5">Mirror Redundancy</h4>
                    <p class="text-[9px] text-muted-foreground uppercase font-bold tracking-tight">Active copies per sector</p>
                  </div>
                  <span class="text-2xl font-black text-primary font-mono">{config.mirror_count}</span>
                </div>
                <input type="range" min="1" max="5" step="2"
                  class="w-full accent-primary bg-muted rounded-full appearance-none h-2.5 cursor-pointer border border-border shadow-inner"
                  bind:value={config.mirror_count} oninput={syncVisualToYaml} />
                <div class="flex justify-between text-[8px] font-black text-muted-foreground/40 uppercase tracking-tighter">
                  <span>1 Copy</span>
                  <span>3 Copies (VOTING)</span>
                  <span>5 Copies</span>
                </div>
              </div>

              <!-- Sector Size -->
              <div class="space-y-4">
                <div class="flex justify-between items-end">
                  <div>
                    <h4 class="text-[10px] font-black uppercase tracking-widest mb-0.5">Sector Geometry</h4>
                    <p class="text-[9px] text-muted-foreground uppercase font-bold tracking-tight">Block alignment (bytes)</p>
                  </div>
                  <span class="text-xl font-black text-primary font-mono">{config.sector_size} <span class="text-[10px] opacity-40 uppercase">B</span></span>
                </div>
                <div class="grid grid-cols-3 gap-2">
                  {#each [512, 1024, 4096] as size}
                    <button 
                      onclick={() => { config.sector_size = size; syncVisualToYaml(); }}
                      class={cn(
                        "py-2 rounded-xl border-2 text-[10px] font-black uppercase tracking-widest transition-all active:translate-y-0.5 shadow-sm",
                        config.sector_size === size ? "bg-primary text-primary-foreground border-primary" : "bg-background border-border hover:border-primary/30"
                      )}>
                      {size}
                    </button>
                  {/each}
                </div>
              </div>

              <div class="grid grid-cols-2 gap-8">
                <!-- Metadata Sectors -->
                <div class="space-y-3">
                  <h4 class="text-[10px] font-black uppercase tracking-widest mb-0.5">Metadata Slots</h4>
                  <div class="flex items-center gap-4">
                    <input type="number" min="2" max="16"
                      class="w-full bg-muted border-2 border-border rounded-xl px-4 py-2 text-xs font-mono font-bold focus:border-primary outline-none"
                      bind:value={config.metadata_sectors} onchange={syncVisualToYaml} />
                  </div>
                </div>

                <!-- Header Size -->
                <div class="space-y-3">
                  <h4 class="text-[10px] font-black uppercase tracking-widest mb-0.5">Header Size</h4>
                  <div class="flex items-center gap-4">
                    <input type="number" min="1" max="4"
                      class="w-full bg-muted border-2 border-border rounded-xl px-4 py-2 text-xs font-mono font-bold focus:border-primary outline-none"
                      bind:value={config.header_size} onchange={syncVisualToYaml} />
                  </div>
                </div>
              </div>

              <!-- Max Bad Sectors -->
              <div class="space-y-3">
                <div class="flex justify-between items-end">
                  <h4 class="text-[10px] font-black uppercase tracking-widest mb-0.5">Blacklist Capacity</h4>
                  <span class="text-xs font-black text-primary font-mono">{config.max_bad_sectors}</span>
                </div>
                <input type="range" min="16" max="256" step="16"
                  class="w-full accent-primary bg-muted rounded-full appearance-none h-2 cursor-pointer border border-border shadow-inner"
                  bind:value={config.max_bad_sectors} oninput={syncVisualToYaml} />
              </div>
            </div>

            <!-- Right Column: Data Types Editor -->
            <div class="space-y-6">
              <div class="flex justify-between items-center border-b border-primary/10 pb-2">
                <h3 class="text-xs font-black uppercase tracking-[0.2em] text-primary">Data Schemas</h3>
                <button 
                  onclick={addDataType}
                  class="p-1.5 rounded-lg bg-primary/10 text-primary hover:bg-primary hover:text-primary-foreground transition-all shadow-sm"
                  title="Add New Data Type">
                  <Plus size={14} />
                </button>
              </div>

              <div class="space-y-6">
                {#each config.data_types as dt, dtIndex}
                  <div class="bg-background border-2 border-border rounded-2xl p-5 shadow-sm relative group">
                    <button 
                      onclick={() => removeDataType(dtIndex)}
                      class="absolute -top-2 -right-2 p-1.5 rounded-lg bg-destructive text-white opacity-0 group-hover:opacity-100 transition-all shadow-md active:translate-y-0.5"
                      title="Remove Data Type">
                      <Trash2 size={12} />
                    </button>

                    <div class="mb-5">
                      <label for="struct-name-{dtIndex}" class="text-[9px] font-black text-muted-foreground uppercase tracking-widest mb-1 block">Struct Name</label>
                      <input 
                        id="struct-name-{dtIndex}"
                        class="w-full bg-muted border border-border rounded-xl px-3 py-1.5 text-[11px] font-mono font-black focus:border-primary outline-none"
                        bind:value={dt.name} oninput={syncVisualToYaml} />
                    </div>

                    <div class="space-y-2">
                      <div class="flex justify-between items-center px-1">
                        <span id="wire-fields-label-{dtIndex}" class="text-[9px] font-black text-muted-foreground uppercase tracking-widest">Wire Fields</span>
                        <button 
                          onclick={() => addField(dtIndex)}
                          aria-labelledby="wire-fields-label-{dtIndex}"
                          class="text-[9px] font-black text-primary hover:underline flex items-center gap-1 uppercase">
                          <Plus size={10} /> Add Field
                        </button>
                      </div>
                      
                      <div class="space-y-1.5">
                        {#each dt.fields as field, fIndex}
                          <div class="flex gap-2 items-center">
                            <input 
                              placeholder="Name"
                              class="flex-1 bg-muted border border-border rounded-lg px-2 py-1 text-[10px] font-mono focus:border-primary outline-none"
                              bind:value={field.name} oninput={syncVisualToYaml} />
                            <select 
                              class="w-24 bg-muted border border-border rounded-lg px-1 py-1 text-[10px] font-mono font-bold focus:border-primary outline-none"
                              bind:value={field.type} onchange={syncVisualToYaml}>
                              <option value="float">float</option>
                              <option value="double">double</option>
                              <option value="int16_t">i16</option>
                              <option value="uint16_t">u16</option>
                              <option value="int32_t">i32</option>
                              <option value="uint32_t">u32</option>
                              <option value="uint8_t">u8</option>
                            </select>
                            <button 
                              onclick={() => removeField(dtIndex, fIndex)}
                              class="p-1.5 text-muted-foreground hover:text-destructive transition-colors">
                              <Trash2 size={12} />
                            </button>
                          </div>
                        {/each}
                        {#if dt.fields.length === 0}
                          <div class="text-center py-4 border border-dashed border-border rounded-xl opacity-30 text-[9px] font-bold uppercase">No fields defined</div>
                        {/if}
                      </div>
                    </div>
                  </div>
                {/each}
                {#if config.data_types.length === 0}
                  <div class="flex flex-col items-center justify-center p-12 border-2 border-dashed border-border rounded-[2rem] opacity-20 space-y-3">
                    <Database size={32} />
                    <p class="text-[10px] uppercase font-black tracking-widest">No Data Types</p>
                  </div>
                {/if}
              </div>
            </div>
          </div>

        </div>
      {:else}
        <div class="h-full flex flex-col gap-4">
          <div class="flex-1 bg-white border-2 border-border p-6 rounded-2xl relative overflow-hidden group shadow-inner">
            <FileJson class="absolute -right-12 -bottom-12 text-muted/20" size={240} strokeWidth={1} />
            <textarea 
              class="w-full h-full bg-transparent text-xs text-foreground font-mono font-bold leading-relaxed resize-none focus:outline-none relative z-10"
              spellcheck="false"
              oninput={syncYamlToVisual}
              bind:value={yamlText}></textarea>
          </div>
        </div>
      {/if}
    </div>

    <footer class="h-24 border-t border-border/60 bg-secondary/40 px-12 flex items-center justify-between shrink-0">
      <div class="max-w-md">
        {#if yamlSaveMsg}
          <div class={cn(
            "text-[10px] font-black uppercase flex items-center gap-3 px-4 py-2 rounded-xl border-2",
            yamlSaveMsg.toLowerCase().includes('success') ? 'bg-green-50 text-accent border-accent/20' : 'bg-red-50 text-destructive border-destructive/20'
          )}>
            {#if yamlSaveMsg.toLowerCase().includes('success')}
              <CheckCircle2 size={16}/>
            {:else}
              <AlertCircle size={16}/>
            {/if}
            {yamlSaveMsg}
          </div>
        {/if}
      </div>
      <button 
        onclick={saveAndRegenerate}
        disabled={yamlSaving}
        class="px-10 py-3 rounded-2xl bg-primary text-primary-foreground hover:bg-foreground transition-all shadow-md active:translate-y-0.5 text-[11px] font-black uppercase tracking-[0.2em]">
        {yamlSaving ? "Processing..." : "[ Regenerate C API ]"}
      </button>
    </footer>
  </div>
{/snippet}

{#snippet SdkView()}
  <div class="flex-1 flex flex-col overflow-hidden">
    <div class="flex-1 overflow-y-auto p-12">
      <div class="max-w-4xl mx-auto space-y-12">
        <div class="bg-primary p-10 rounded-[2.5rem] text-primary-foreground shadow-lg flex gap-10 items-center relative overflow-hidden">
          <Code class="absolute -right-8 -bottom-8 opacity-10" size={200} />
          <div class="w-20 h-20 rounded-3xl bg-background/20 backdrop-blur-sm flex items-center justify-center shrink-0 border border-white/10">
            <Code size={40} />
          </div>
          <div class="relative z-10">
            <h2 class="text-2xl font-black uppercase tracking-tight mb-2">Guided SDK Generator</h2>
            <p class="text-xs font-bold opacity-80 leading-relaxed max-w-xl uppercase tracking-tighter">
              Produce hardened C integration layers with zero boilerplate. 
              Each generated source includes mandatory hardware guards and bus mapping.
            </p>
          </div>
        </div>

        <div class="grid grid-cols-1 md:grid-cols-3 gap-12">
          <div class="space-y-8 md:col-span-1">
            <div class="space-y-3">
              <label for="sensor-id" class="text-[10px] font-black uppercase text-muted-foreground tracking-widest px-1">Hardware ID</label>
              <input 
                id="sensor-id"
                class="w-full bg-muted border-2 border-border rounded-2xl px-5 py-3 text-xs focus:border-primary outline-none transition-all font-mono font-bold shadow-inner"
                bind:value={sensorName} placeholder="e.g. bme280" />
            </div>

            <fieldset class="space-y-3">
              <legend class="text-[10px] font-black uppercase text-muted-foreground tracking-widest px-1">Bus Protocol</legend>
              <div class="grid grid-cols-2 gap-3">
                {#each ['spi', 'i2c', 'uart', 'custom'] as bus}
                  <button 
                    onclick={() => busType = bus}
                    class={cn(
                      "py-2.5 rounded-2xl border-2 text-[10px] font-black uppercase tracking-widest transition-all shadow-sm active:translate-y-0.5",
                      busType === bus ? "bg-primary text-primary-foreground border-primary" : "bg-background border-border hover:border-primary/30"
                    )}>
                    {bus}
                  </button>
                {/each}
              </div>
            </fieldset>

            <button 
              onclick={generateSdk}
              disabled={sdkGenerating}
              class="w-full py-4 rounded-2xl bg-accent text-accent-foreground hover:bg-foreground hover:text-white transition-all shadow-lg active:translate-y-0.5 text-[11px] font-black uppercase tracking-[0.2em]">
              {sdkGenerating ? "Working..." : "[ Create SDK Package ]"}
            </button>
          </div>

          <div class="md:col-span-2 flex flex-col min-h-[460px]">
            {#if Object.keys(sdkFiles).length > 0}
              <div class="flex-1 flex flex-col bg-white rounded-[2rem] border-2 border-border overflow-hidden shadow-inner relative">
                <div class="flex bg-secondary/50 border-b border-border/60 overflow-x-auto no-scrollbar p-1">
                  {#each Object.keys(sdkFiles) as fname}
                    <button 
                      onclick={() => sdkActiveFile = fname}
                      class={cn(
                        "px-5 py-2 text-[10px] font-black font-mono transition-all rounded-xl",
                        sdkActiveFile === fname ? "bg-primary text-primary-foreground shadow-sm" : "text-muted-foreground hover:text-foreground"
                      )}>
                      {fname}
                    </button>
                  {/each}
                </div>
                <div class="flex-1 p-8 overflow-auto bg-muted/10">
                  <pre class="text-[11px] leading-relaxed text-foreground/80 font-mono font-bold whitespace-pre">{sdkFiles[sdkActiveFile] ?? ""}</pre>
                </div>
                <div class="absolute bottom-4 right-6">
                   <div class="text-[8px] font-black text-primary uppercase bg-primary/10 px-2 py-1 rounded">Generated Package</div>
                </div>
              </div>
            {:else}
              <div class="flex-1 flex flex-col items-center justify-center border-4 border-dotted border-border/50 rounded-[2rem] opacity-20 gap-6">
                <Code size={64} strokeWidth={1} />
                <p class="text-[11px] font-black uppercase tracking-[0.3em]">Module Sandbox Empty</p>
              </div>
            {/if}
          </div>
        </div>
      </div>
    </div>
  </div>
{/snippet}

<style>
  /* Custom scrollbar for GNOME aesthetic */
  :global(::-webkit-scrollbar) {
    width: 8px;
    height: 8px;
  }
  :global(::-webkit-scrollbar-track) {
    background: transparent;
  }
  :global(::-webkit-scrollbar-thumb) {
    background: #d6d3d1;
    border-radius: 10px;
    border: 2px solid #fdfbf7;
  }
  :global(::-webkit-scrollbar-thumb:hover) {
    background: #a8a29e;
  }

  /* Hide scrollbar for Chrome, Safari and Opera */
  .no-scrollbar::-webkit-scrollbar {
    display: none;
  }

  /* Hide scrollbar for IE, Edge and Firefox */
  .no-scrollbar {
    -ms-overflow-style: none;  /* IE and Edge */
    scrollbar-width: none;  /* Firefox */
  }

  /* Focus rings for GNOME accessibility */
  :global(button:focus-visible), :global(input:focus-visible), :global(textarea:focus-visible) {
    outline: 2px solid var(--color-primary);
    outline-offset: 2px;
  }

  /* Slider thumb style for retro feel */
  input[type=range]::-webkit-slider-thumb {
    height: 24px;
    width: 24px;
    border-radius: 8px;
    background: #44403c;
    cursor: pointer;
    -webkit-appearance: none;
    margin-top: -6px;
    box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    border: 2px solid #fdfbf7;
  }
  
  input[type=range]::-webkit-slider-runnable-track {
    background: #e7e5e4;
    border-radius: 10px;
    height: 12px;
    box-shadow: inset 0 1px 3px rgba(0,0,0,0.1);
  }
</style>
