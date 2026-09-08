# Open-Weight AI Transition Plan & Conversation Archive
**Date:** September 2026  
**Environment:** Neovim / Terminal / Cybersecurity & Pen-Testing Workflows  

---

## 1. Executive Summary & Core Conclusion
No single open-weight model uniformly outperforms Claude Opus 5 across all general benchmarks. However, a strategically orchestrated **"Router & Specialize" multi-model open-weight stack** can actively match or exceed Opus 5 on specific software engineering, low-level logic, and terminal-agent automation tasks—all while dropping API costs down to fractions of a cent per token.

### The Frontrunners at a Glance
| Model | Size (Total / Active) | Core Strength | Context Window | Key Benchmark / Metric |
| :--- | :--- | :--- | :--- | :--- |
| **Claude Opus 5** *(Proprietary)* | *Undisclosed* | SOTA Agentic Workflows & Multi-Step Logic | 1 Million | **82.1/100** BenchLM Score |
| **Kimi K3** *(Open Weight)* | 2.8T / 104B | Scientific Reasoning & Web Browsing | 1 Million | **93.5%** GPQA Diamond |
| **DeepSeek V4 Pro** *(Open Weight)* | 1.6T | Agentic Coding & Repository Mapping | 1 Million | **80.6%** SWE-Bench |
| **GLM-5.3** *(Open Weight)* | 753B / 40B | Cost-Effective Frontier Logic | 1 Million | **83.9%** Terminal-Bench |
| **GLM-5.3-Flash** *(Open Weight)* | 320B / 18B | Speed, Efficiency & API Value | 1 Million | **84.3%** Terminal-Bench |
| **Gemma 4 31B** *(Open Weight)* | 31B Dense | Local Desktop / Laptop Deployment | 256K | Fits on 1x RTX 4090 GPU |

---

## 2. The Phased Local Hardware & API Roadmap

### Phase 1: The "Zero-Cost Hardware" Hybrid Setup (Current)
* **Hardware:** Existing NVIDIA RTX 2070 (8GB VRAM).
* **Local Tier ($0):** Run **Qwen2.5-Coder-7B-Instruct (Q4_K_M)** via Ollama for inline buffer completions and minor scripting.
* **Cloud Tier (Pay-As-You-Go):** 
  * **Kimi K3** (Direct Moonshot API) for back-and-forth architectural discussions, decompilation analysis, and note-taking.
  * **DeepSeek V4 Pro** (Direct DeepSeek API) integrated with **Aider** for large multi-file codebase building.
* **Cost Projection:** Hardware: **$0**. API usage: **~$4 to $12 / month** (leveraging DeepSeek's aggressive context caching).

### Phase 2: The "Single Consumer Flagship" Upgrade (Short-Term)
* **Hardware:** Upgrade to a single 24GB VRAM footprint (**RTX 3090** used, or **RTX 4090/5090** new).
* **Local Tier ($0):** Run **DeepSeek-R1-Distill-Qwen-32B** or **Qwen3-Coder-32B (Q4_K_M)** locally at 40-60 tokens/sec. This handles deep, complex local reasoning and single-file refactoring entirely offline.
* **Cloud Tier:** Retain Kimi K3 and DeepSeek V4 Pro APIs strictly for whole-project macro generation.
* **Cost Projection:** Hardware: **$650 - $2,000**. API usage: **~$1 to $3 / month**.

### Phase 3: The "Local MoE Pro-Sumption" Workstation (Mid-Term)
* **Hardware:** Multi-GPU PC build (4x or 5x RTX 3090/4090s via PCIe risers) OR an **Apple Mac Studio (M4/M5 Ultra) with 192GB Unified Memory**.
* **Local Tier ($0):** Load a 3-bit or 4-bit quantization of **GLM-5.3-Flash** or **DeepSeek V4-Flash (300B+ MoE)** completely into local memory.
* **Impact:** Achieve full, offline parity with proprietary tier-1 systems. Drop a 100k-token repository map into local memory and code full features offline.
* **Cost Projection:** Hardware: **$3,500 - $5,600+**. API usage: **<$1.00 / month**.

### Phase 4: The "Impossible" Data Center Tier (Enterprise Scale)
* **Hardware:** 8x NVIDIA H100 / H200 or B200 NVL72 Node with 3-phase industrial power and dedicated data center cooling.
* **Local Tier ($0):** Host unquantized, raw FP16 instances of **Kimi K3 (2.8T)** and **Qwen 3.8 Max (2.4T)**.
* **Cost Projection:** Hardware: **$250,000 - $350,000+**. API usage: **$0** (High idle electricity costs).

---

## 3. Resolving Token Compaction & Price Surges
In tools like `claude-code`, reaching the 1M limit triggers compaction, which alters the conversation history prefix, **breaks prompt caching**, and hits you with full-price Cache Misses on your entire codebase context.

### The Open-Weight Advantage:
1. **Aggressive Cache Ingestion:** Direct developer endpoints like DeepSeek charge **~$0.435 per million tokens for a cache miss**, but drop down to an incredibly cheap **~$0.003625 per million tokens for a cache hit**. Processing 800k tokens of persistent history costs a fraction of a cent per turn.
2. **Structural Context Isolation (Aider):** Using terminal-centric tools like **Aider** limits full context filling. Aider builds a tree-sitter based PageRank repository map to pass compressed code structural syntax rather than swallowing whole files. Developers can also manually purge context bloat via `/drop` commands without invalidating the active prefix cache history.

---

## 4. Cybersecurity, Decompilations & Freedom from Model Throttling
Proprietary front-ends often employ non-transparent guardrails that down-grade processing tiers (e.g., dropping from Opus 5 to 4.8) when encountering blocks of assembly, malformed web headers (e.g., mismatched `Content-Length`), or protocol logs.

### The Developer API Difference:
* **Fixed Endpoint Stability:** Connecting via official developer platforms (**api.deepseek.com** or **api.moonshot.ai**) means your requested model ID is locked. The platform can reject a request if it violates core rules, but it will **never stealthily downgrade** the active processing intelligence.
* **Permissive Architectural Parsing:** DeepSeek V4 Pro and GLM-5.3 treat decompiled structures, raw C code pointer adjustments (`memcpy`/`strcpy`), and malformed packet distributions as standard engineering debugging problems.
* **Uncensored Weights (Phases 2/3):** Running distilled or custom models locally gives you access to open-source "unaligned" or "uncensored" community fine-tunes. This allows you to evaluate active vulnerability analysis, shellcode execution, and red-team simulations offline without corporate gatekeeping or data telemetry sharing.

---

## 5. Technical Configuration for Neovim (CodeCompanion.nvim)
The following snippet hooks your Neovim workspace up to your free, local 7B completion engine via Ollama, while routing your deeper side-chat panels directly to your uncensored DeepSeek Pro endpoint:

```lua
require("codecompanion").setup({
  strategies = {
    chat = {
      adapter = "deepseek_pro",
    },
    inline = {
      adapter = "qwen_local",
    },
  },
  adapters = {
    deepseek_pro = function()
      return require("codecompanion.adapters").extend("openai", {
        env = {
          api_key = "YOUR_DIRECT_DEEPSEEK_KEY",
        },
        url = "https://api.deepseek.com/v1/chat/completions",
        schema = {
          model = {
            default = "deepseek-v4-pro",
          },
        },
      })
    end,
    qwen_local = function()
      return require("codecompanion.adapters").extend("ollama", {
        schema = {
          model = {
            default = "qwen2.5-coder:7b",
          },
        },
      })
    end,
  },
})
```
