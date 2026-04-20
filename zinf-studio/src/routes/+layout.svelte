<script lang="ts">
  import "../app.css";
  let { children } = $props();
</script>

<svelte:head>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=JetBrains+Mono:ital,wght@0,100..800;1,100..800&display=swap');
  </style>
</svelte:head>

<div class="h-screen w-screen overflow-hidden bg-background text-foreground flex flex-col antialiased">
  {@render children()}
</div>
