# Basic Chatting about Cards

## Prompt

when are the next rounds of GPUs going to come out


## Response

The next major rounds of consumer gaming GPUs from Nvidia and AMD are expected to arrive between late 2027 and 2028 due to industry production shifts and component shortages. [1]
### Nvidia Release Schedule
Nvidia is reportedly skipping new consumer gaming desktop GPU architectures for the year. [2, 3]

As reported by Tom's Hardware, "Nvidia's next generation RTX 60 series GPUs are now likely begin production in 2028." [4]

* Rubin-based gaming cards are projected to start arriving around late 2027 or early 2028.
* Planned Super refreshes have faced delays or shelfing. [3, 5]

### AMD Release Schedule
AMD's next-generation graphics lineup follows a similar delayed trajectory.

According to Tom's Hardware, "manufacturers expect new Team Red cards in the second half of 2027 alongside Nvidia." [5]

* RDNA 5 cards are anticipated anywhere from mid-2027 to early 2028.
* This creates a longer gap following the initial RDNA 4 rollouts. [5, 6]

### Why Releases Are Pushed Back

* Memory Shortages: High-density DRAM and GDDR memory are in tight supply.
* AI Prioritization: Factories prioritize high-margin AI accelerators and data center silicon over consumer gaming cards.
* Track upcoming roadmaps directly via [TechPowerUp](https://www.techpowerup.com/review/future-hardware-releases/). [3, 7]

Are you looking to upgrade your current system right now, or are you trying to decide whether it is worth waiting for these upcoming releases?

[1] [https://www.reddit.com](https://www.reddit.com/r/GamingLeaksAndRumours/comments/1tzmanl/next_generation_gaming_gpus_from_nvidia_and_amd/)
[2] [https://tech-insider.org](https://tech-insider.org/nvidia-skips-2026-gaming-gpus/)
[3] [https://gaming.lenovo.com](https://gaming.lenovo.com)
[4] [https://www.tomshardware.com](https://www.tomshardware.com/pc-components/gpus/report-claims-nvidia-will-not-be-releasing-any-new-rtx-gaming-gpus-in-2026-rtx-60-series-likely-debuting-in-2028)
[5] [https://www.tomshardware.com](https://www.tomshardware.com/pc-components/gpus/amds-rdna-5-gaming-gpus-are-coming-late-next-year-according-to-aibs-at-computex-manufacturers-expect-new-team-red-cards-in-the-second-half-of-2027-alongside-nvidia)
[6] [https://www.digitalfoundry.net](https://www.digitalfoundry.net/news/2026/06/next-gen-gpu-rumour-round-up-amd-rdna-5-and-nvidia-60-series-delayed-until-mid-2027-or-2028-but-rtx-50-super-coming-sooner)
[7] [https://www.techpowerup.com](https://www.techpowerup.com/review/future-hardware-releases/)


## Prompt

AMD GPU vs Nvidia

## Response

The choice between AMD and Nvidia comes down to whether you prioritize pure raw performance per dollar (AMD) or cutting-edge visual features and software ecosystems (Nvidia).
### At a Glance Comparison
For a fast overview of how they generally stack up across core metrics:

| Feature / Metric | 🔴 AMD Radeon | 🟢 Nvidia GeForce |
|---|---|---|
| Value (Price-to-Performance) | Winner (More VRAM and higher raw frame rates for less money) | Expensive premiums on middle-to-high tier hardware |
| Ray Tracing | Lagging behind; heavy performance hits | Winner (Dedicated RT cores handle demanding path/ray tracing) |
| Upscaling Tech | FSR (Spatial; works on all cards but has more visual artifacts) | Winner (DLSS; AI-driven tensor cores offer cleaner upscaling) |
| VRAM Capacity | Generous allocations across all pricing brackets | Often restrictive or stingy on mid-range models |
| Productivity & AI Workloads | Viable for basic workflows; improving open-source stack | Industry Standard (CUDA ecosystem dominates 3D rendering and machine learning) |
| Power Efficiency | Higher power draw on high-end models | Better overall architecture efficiency |

------------------------------
### Core Strengths of AMD Radeon

* More Budget-Friendly Frame Rates: If you just want maximum, native frame rates at 1080p or 1440p without flipping on modern bells and whistles, AMD wins.
* Longer Lifespan via VRAM: AMD reliably packs more Video RAM into lower price brackets. This stops newer, unoptimized games from hitting abrupt performance walls due to maxed-out memory limit allocations.

### Core Strengths of Nvidia GeForce

* Superior Up-Sampling & Frame Gen: DLSS uses physical AI tensor hardware inside the chip. It looks substantially sharper and suffers from fewer digital artifacts or "ghosting" anomalies than AMD's software-based alternative, FSR.
* Unmatched Professional Utility: If your PC doubles as a workstation for Blender 3D rendering, Adobe creative suites, or training local AI language models, Nvidia's proprietary CUDA framework is non-negotiable.

Are you looking to buy a GPU for strict gaming purposes, or do you also intend to do 3D rendering, video editing, or AI workloads?


## Prompt

gaming purposes, 3D rendering, video editing, and AI workloads

## Response

Because your workflow actively spans 3D rendering, video editing, and local AI workloads alongside gaming, Nvidia is the unequivocal winner.
While AMD offers phenomenal raw performance per dollar for traditional gaming, professional and creative productivity pipelines remain heavily anchored to Nvidia's proprietary CUDA ecosystem. Many industry-standard applications run significantly faster on Nvidia, and some AI or rendering tools outright require it. [1, 2, 3]
------------------------------
### How They Compare Across Your Specific Workloads

### 1. 3D Rendering (Blender, Maya, Redshift, Unreal Engine)

* 🟢 Nvidia: Absolute Leader. As noted in community benchmarks, “Blender and Maya will have the largest benefit, as in potentially multiple times the performance.” Nvidia's OptiX API and dedicated ray-tracing (RT) cores crush AMD in viewport fluidness and final render times. In fact, independent data tracking shows “No AMD GPU is in the top 25. A 7900 XTX places 37th...” [3]
* 🔴 AMD: Viable for basic workflows, but heavily limited by software optimization. Even with recent driver updates, “Redshift continues to heavily favor NVIDIA hardware... NVIDIA GPUs still deliver substantially faster render times” [4]

### 2. AI & Machine Learning Workloads

* 🟢 Nvidia: The Industry Standard. Nearly all modern AI frameworks—like PyTorch, TensorFlow, Stable Diffusion, and local LLM tools (LM Studio, Ollama)—are written natively for Nvidia CUDA. You get "plug-and-play" compatibility out of the box. [2, 3]
* 🔴 AMD: Getting better but requires work. To run local AI on AMD, you often have to rely on their ROCm framework (which historically prefers Linux over Windows) or translation layers like Vulkan/DirectML. [2, 5]

### 3. Video Editing (DaVinci Resolve, Premiere Pro, After Effects)

* 🟢 Nvidia: Winner for Timeline Smoothness. Nvidia’s NVENC encoder handles heavy, uncompressed timelines brilliantly. The latest graphics cards feature hardware acceleration for demanding formats like 10-bit 4:2:2 video. Video editors report that cards like the “5080 is far smoother for video editing... handles everything so smoothly (without proxies).”
* 🔴 AMD: Highly capable at basic video encoding, but you are much more likely to hit timeline stutters or require proxy files when editing high-bitrate 4K or 8K footage. [1, 6, 7, 8]

### 4. Gaming

* 🟢 Nvidia: Dominates in heavy ray-tracing or path-tracing titles (like [Cyberpunk 2077](https://www.google.com/search?q=cyberpunk+2077&kgmid=/m/0kvfm9q) or [Alan Wake 2](https://www.google.com/search?q=alan+wake+2&kgmid=/g/11ckscyy_x)). It also gives you access to DLSS, which offers superior upscaling and multi-frame generation image clarity compared to AMD's FSR.
* 🔴 AMD: Gives you more traditional rasterized frames per dollar and a larger allotment of VRAM at lower pricing tiers. [3, 7, 9, 10]

------------------------------
### Top GPU Recommendations for Your Workload
If you are looking to build or buy a system, these specific models are the best targets for a heavy hybrid workload:

* Nvidia GeForce RTX 4090 — Best Overall Performance: The uncontested holy grail for creators. Its massive 24 GB GDDR6X VRAM provides the huge memory buffer required to train larger local AI models and render highly complex 3D scenes without crashing.
* [Nvidia GeForce RTX 4080 Super](https://www.google.com/search?q=Gigabyte+GAMING+GeForce+RTX+4080+SUPER+OC+16G+nvidia+16+gb+GDDR6X&ibp=oshop&pvorigin=29&prds=catalogid:7368149937455336273,productid:7599634531270933885,imageDocid:4473259041201428616,gpcid:15977444690952400441,pvt:hg,pvo:29,headlineOfferDocid:1013480991562038203) — The High-End Sweet Spot: Offers a superb balance of professional rendering speed and top-tier 4K gaming performance without hitting the extreme pricing tier of the 4090.
* [Nvidia GeForce RTX 5070 Ti](https://www.google.com/search?q=PNY+NVIDIA+GeForce+RTX+5070+Ti+OC+Triple+Fan+Graphics+Card&ibp=oshop&pvorigin=29&prds=catalogid:6800775342455117360,productid:10779623886128682186,imageDocid:17583499749116423350,gpcid:5873149452505727939,pvt:hg,pvo:29,headlineOfferDocid:4687015764614655869) — Best Mid-Range Workhorse: An excellent modern option featuring 16GB of fast GDDR7 memory. The 16GB frame buffer is highly capable of handling heavy 3D rendering and creative multitasking.

Could you share your approximate budget or let me know which software applications (e.g., Blender, DaVinci, Premiere) you spend the most hours using? I can pinpoint the exact GPU tier that gives you the best return on your investment.

[1] [https://www.reddit.com](https://www.reddit.com/r/buildapc/comments/1ju4qp5/is_amd_finally_on_par_with_nvidia_for_3d/)
[2] [https://jarvislabs.ai](https://jarvislabs.ai/ai-faqs/what-is-the-difference-between-amd-and-nvidia-gpus)
[3] [https://www.quora.com](https://www.quora.com/Which-GPU-is-better-for-rendering-3D-stuff-AMD-or-Nvidia)
[4] [https://www.szwecent.com](https://www.szwecent.com/graphics-card-manufacturer-selection-nvidia-vs-amd/)
[5] [https://www.youtube.com](https://www.youtube.com/watch?v=ywHYY0zKeBo)
[6] [https://www.youtube.com](https://www.youtube.com/watch?v=JVGYuYkhjDA&t=6)
[7] [https://www.techwisezoneblog.com](https://www.techwisezoneblog.com/amd-gpu-vs-nvidia-which-graphics-card-is-better-in-2026/)
[8] [https://www.reddit.com](https://www.reddit.com/r/davinciresolve/comments/1uproki/nvidia_vs_amd_in_2026_what_is_the_better_card_for/)
[9] [https://www.youtube.com](https://www.youtube.com/watch?v=wEN9mE8m_vM&t=452)
[10] [https://www.davescomputers.com](https://www.davescomputers.com/amd-vs-nvidia-graphics-cards-2026-nj/)


## Prompt

Possible to combine 4080 and 5070 for gaming and workflows?


## Response

Yes, you can physically combine an RTX 4080 and an RTX 5070 in the same system, but they will work very differently depending on whether you are gaming or doing heavy workflow tasks.
Modern PCs no longer use "SLI" bridges to combine graphics cards into a single super-GPU. Instead, your computer will treat them as two independent processing units living inside the same machine. [1, 2]
------------------------------
### 1. For 3D Rendering & AI Workloads (⭐⭐⭐⭐⭐ - Excellent)
This is where a dual-GPU setup shines. Many professional applications can use both cards simultaneously to massively slash rendering and training times.

* 3D Rendering (Blender Cycles, Octane, V-Ray): Blender can render one scene using both cards at the same time. It assigns different sections (tiles) of the image to each card simultaneously, essentially cutting your render times significantly. Note: VRAM does not stack. If you use a base 12GB RTX 5070 and a 16GB RTX 4080, your maximum project file size will be limited to the lowest card's VRAM pool (12GB). [3, 4, 5, 6]
* AI Workloads (LLMs & Stable Diffusion): You can distribute AI tasks across both cards. For instance, you can load a large local language model entirely into the combined memory pools or use the RTX 5070’s cutting-edge FP4 tensor acceleration to handle modern quantization workloads while the 4080 handles heavy image generation batching. [7, 8]

### 2. For Video Editing (⭐⭐⭐ - Good, with Caveats)

* Applications like DaVinci Resolve Studio (paid version) natively support dual GPUs and will leverage both cards for timeline playback and heavy effects rendering.
* However, programs like Adobe Premiere Pro are heavily optimized to rely primarily on a single "main" card for live previewing. They will only pass off specific export actions to the secondary card.

### 3. For Gaming (⭐ - Poor / No Benefit)

* Zero Combined Performance: Modern video games cannot use two different GPUs to increase your frame rates.
* Dedicated Task Splitting: The only gaming benefit to this setup is that you can plug your main monitor into one card (e.g., the RTX 4080) for gaming, and use the secondary card (the RTX 5070) to handle background tasks like streaming encoding (OBS) or recording without losing a single frame of gameplay. [6]

------------------------------
### ⚠️ Crucial Hardware Requirements Before You Do It
If you want to build this dual-GPU setup, you must verify three hardware requirements to avoid crashing or damaging your parts:

   1. Massive Power Supply (PSU): An RTX 4080 draws up to 320W and an RTX 5070 draws around 250W. Combined with a high-end CPU and system components, you will absolutely need a tier-one 1000W to 1200W power supply to handle the spikes safely.
   2. Motherboard Space (PCIe Lanes): You need a motherboard with at least two physical PCIe x16 slots that are spaced far enough apart. Modern GPUs are thick (often 3 to 4 slots wide); fitting both on a standard motherboard without choking the fans for airflow requires a large case and careful planning.
   3. Thermal Management: Running two high-draw cards right next to each other creates an immense amount of ambient heat. You will need aggressive case fan configurations to prevent thermal throttling. [9]

Would you like help calculating whether your current power supply and motherboard can support adding a second card, or would you like to review how VRAM pooling works across different generations?

[1] [https://www.youtube.com](https://www.youtube.com/watch?v=hBlL2ZFRWsI)
[2] [https://www.youtube.com](https://www.youtube.com/watch?v=R7pptUipV94)
[3] [https://blender.stackexchange.com](https://blender.stackexchange.com/questions/68257/does-blender-support-dual-gpu-rendering-if-the-two-gpus-arent-the-same)
[4] [https://www.quora.com](https://www.quora.com/Can-you-use-multiple-GPUs-for-one-computer-in-Blender)
[5] [https://irendering.net](https://irendering.net/render-faster-with-multi-gpu-in-blender-cycles/)
[6] [https://www.reddit.com](https://www.reddit.com/r/blender/comments/zfsi51/2_different_gpus/)
[7] [https://www.youtube.com](https://www.youtube.com/watch?v=o25ZHnHYuts)
[8] [https://www.youtube.com](https://www.youtube.com/watch?v=KFoV3CKXKgo&t=370)
[9] [https://www.pugetsystems.com](https://www.pugetsystems.com/labs/articles/nvidia-geforce-rtx-5070-content-creation-review/)


===========================================================================
===========================================================================
===========================================================================
===========================================================================


## Prompt

top-of-the-line graphics card for both gaming and LLM usage


## Response

The top-of-the-line consumer graphics card for combining maximum 4K gaming performance with local Large Language Model (LLM) execution is the NVIDIA GeForce RTX 5090 (32GB GDDR7). [1, 2]
### Why the RTX 5090 is the Ultimate Dual-Purpose Flagship

* Unmatched VRAM Capacity: Packing 32GB of ultra-fast GDDR7 memory, it provides the largest memory pool available on a consumer card, which is essential for loading larger local LLMs (like quantized 70B parameter models) without offloading tasks to a slower CPU. [2, 3]
* Absolute Gaming Dominance: As NVIDIA's top-tier consumer GPU built on the Blackwell architecture, it handles maxed-out 4K gaming, heavy ray tracing, and DLSS frame generation effortlessly. [2, 4]
* Ecosystem Maturity: NVIDIA's CUDA and TensorRT support ensures plug-and-play compatibility with popular local AI interfaces like Ollama, LM Studio, and llama.cpp. [5, 6]

### Alternative Elite Options

* The Professional Alternative ([NVIDIA RTX PRO 6000 Blackwell](https://www.bhphotovideo.com/c/product/1895402-REG/nvidia_900_5g144_2200_000_rtx_pro_6000_blackwell.html)): If your LLM work is enterprise-grade and you need 96GB of VRAM to run massive models entirely on a single card, look to the professional workstation line. However, it costs significantly more and is not optimized or priced for recreational gaming. [7, 8]
* The Budget-Conscious Sweet Spot (NVIDIA GeForce RTX 5080): Featuring 16GB of VRAM, the RTX 5080 handles mainstream local AI experimentation and high-refresh-rate gaming at a fraction of the flagship's price. [9, 10]

If you'd like, let me know:

* Your target budget
* The exact size of the LLMs (e.g., 8B, 32B, 70B) you plan to run locally

I can help you decide whether the RTX 5090 fits your setup or if a lower-tier card or multi-GPU configuration makes more sense.

[1] [https://www.tomshardware.com](https://www.tomshardware.com/reviews/best-gpus,4380.html)
[2] [https://www.newegg.com](https://www.newegg.com/insider/best-gpus-for-ai-and-local-llms-in-2026-vram-is-king/)
[3] [https://www.hostrunway.com](https://www.hostrunway.com/blog/best-gpu-for-running-local-llms-and-private-ai-in-2026-complete-buyers-guide-ollama-lm-studio-llama-cpp/)
[4] [https://www.youtube.com](https://www.youtube.com/watch?v=wEN9mE8m_vM&t=276)
[5] [https://www.youtube.com](https://www.youtube.com/watch?v=qmMcSjwi-tQ)
[6] [https://digibuggy.com](https://digibuggy.com/blog-details/best-gpus-for-machine-learning-in-2026)
[7] [https://www.reddit.com](https://www.reddit.com/r/LocalLLaMA/comments/1pihhd5/best_gpu_for_running_local_llms/)
[8] [https://millionminer.com](https://millionminer.com/news/best-gpu-for-running-llms-locally)
[9] [https://www.newegg.com](https://www.newegg.com/insider/best-gpus-for-1440p-gaming-in-2026-top-picks-for-every-budget/)
[10] [https://www.newegg.com](https://www.newegg.com/insider/best-gpus-for-ai-and-local-llms-in-2026-vram-is-king/)


## Prompt

I don't want to spend more than $3000, but $1500 is my sweet spot. I don't mind housing multiple cards.

## Response

With a budget limit of $3,000 and a $1,500 sweet spot, the current GPU market dynamics radically reshape your strategy. Due to intense AI infrastructure demand crushing retail supply, online street prices for a single RTX 5090 have skyrocketed past $5,000 (far above its $1,999 MSRP). [1, 2, 3, 4]
Because you are open to housing multiple cards, you can bypass these inflated consumer flagship prices. A multi-GPU build will actually give you more VRAM for your dollar, which is the gold standard for running local LLMs. [5, 6]
Here are the two best multi-GPU configurations tailored to your budget constraints:
### 1. The $1,500 Sweet Spot: Dual Used RTX 3090 (24GB x 2 = 48GB VRAM)
If you want to stick strictly to your preferred budget, hunting the used market for two NVIDIA GeForce RTX 3090 cards is the undisputed king of price-to-performance for AI. [5]

* The Cost: Used RTX 3090s routinely sell for $700 to $800 each, perfectly landing your total at roughly $1,400 to $1,600.
* The AI Benefit: You get a massive 48GB of combined VRAM. This allows you to natively run heavy 70B parameter models (like Llama 3 70B) at highly usable token speeds without any CPU offloading bottlenecks. [6]
* The Gaming Catch: Since SLI/NVLink is dead for modern gaming, only one RTX 3090 will handle your games. While it is an older card, a single 3090 still delivers excellent high-end 4K gaming performance. [5]

### 2. The $3,000 Max Performance Rig: Dual RTX 5080 (16GB x 2 = 32GB VRAM)
If you want modern architecture with full support for DLSS 5 and the blistering speed of GDDR7 memory, buying two current-gen cards maximizes your budget. [1, 7]

* The Cost: Retail street prices for the [RTX 5080](https://www.google.com/search?q=Gigabyte+Technology+Gv+N5080aorusif+Wd+16gd&ibp=oshop&pvorigin=29&prds=catalogid:10676356249752820141,productid:4999494647939347638,imageDocid:2142610165546825738,gpcid:9838168915000605328,pvt:hg,pvo:29,headlineOfferDocid:4261016522024440662) hover around $1,580 to $1,700. Snagging two will put you right up against your $3,000 to $3,200 upper limit.
* The AI Benefit: 32GB of cutting-edge GDDR7 VRAM across two cards. This handles medium-to-large models flawlessly and loads data lightning fast.
* The Gaming Benefit: A single RTX 5080 handles top-tier 4K gaming, giving you a massive generational leap over the 30-series for modern ray-traced titles. [7]

------------------------------
### Direct Strategy Comparison

| Setup | Approx. Total Cost | Total AI VRAM | Gaming Capability | Best For |
|---|---|---|---|---|
| 2x Used RTX 3090 | ~$1,500 | 48 GB | Great 4K (Single GPU) | Maximum LLM capacity on a budget. |
| 2x New RTX 5080 | ~$3,100 | 32 GB | Elite 4K / DLSS 5 (Single GPU) | Future-proof gaming & high-speed AI tracking. |

### ⚠️ Critical Multi-GPU Caveats to Keep in Mind
Before committing to a dual-card machine, you must ensure your PC skeleton can handle it:

* The Motherboard Requirement: You need a motherboard that supports x8/x8 PCIe lane splitting. Most standard consumer boards drop the second slot to a bottlenecked x4 speed if two cards are plugged in.
* The Power Grid: A dual RTX 3090 or dual RTX 5080 setup pulls serious power. You will need a premium 1300W to 1600W Titanium Power Supply (PSU) and ideally a dedicated 15-amp or 20-amp wall circuit so you don't trip your breakers.
* Physical Space & Heat: These are thick cards. You will likely need a full-tower case with excellent airflow or blower-style models so the top card doesn't starve for air. [6, 8, 9]

To help map out the physical build, let me know:

* What Power Supply (PSU) and Motherboard you currently have.
* Whether your case has the room to physically fit two massive three-slot graphics cards.


[1] [https://www.tomshardware.com](https://www.tomshardware.com/pc-components/gpus/nvidia-returns-to-selling-founders-edition-rtx-50-series-gpus-at-msrp-in-person-at-pax-west-verified-priority-access-has-rtx-5090-rtx-5080-and-rtx-5070-at-list-price)
[2] [https://finance.biggo.com](https://finance.biggo.com/news/f1c72b65-a71b-48f2-8ff0-0cb775d1a797)
[3] [https://www.reddit.com](https://www.reddit.com/r/gpu/comments/1w8x6w2/nvidia_is_back_in_selling_rtx_50_at_msrp_but_only/)
[4] [https://www.techpowerup.com](https://www.techpowerup.com/352387/nvidia-offers-geforce-rtx-50-series-founders-edition-gpus-at-msrp-at-pax-west)
[5] [https://www.reddit.com](https://www.reddit.com/r/LocalLLM/comments/1iud9f0/best_priceperformancepower_for_a_1500_budget/)
[6] [https://www.gmicloud.ai](https://www.gmicloud.ai/en/blog/best-budget-gpu-llm-used-consumer-cards)
[7] [https://www.nvidia.com](https://www.nvidia.com/en-us/geforce/graphics-cards/50-series/rtx-5080/)
[8] [https://build.nvidia.com](https://build.nvidia.com/rtx/multi-gpu)
[9] [https://www.gmicloud.ai](https://www.gmicloud.ai/en/blog/best-gpu-local-llm-consumer-pro-comparison)

