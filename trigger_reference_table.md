# Trigger Reference Table — CBBOAC Formulas, IDs & SGs

Source: "Copy of Orderflow Triggers" Google Doc.
ID56 (Range) and ID15 (Renko 6t) = account/session validation — excluded from signal analysis.
IDs are chart-specific: same number = different study on a different chart window.

---

## Full Trigger List

| # | Used | Trigger | Dir | Chart | CBBOAC Formula | IDs Referenced | Description |
|---|------|---------|-----|-------|----------------|----------------|-------------|
| 1 | — | Fading MOMO Below Candle | Bull | Range | `AND(ID56.SG1=1, ID13.SG1[-2]<ID13.SG1[-1], ID13.SG1[-1]<ID13.SG1, ID13.SG1<0)` | ID13.SG1 | Delta rising 3 bars but still negative — fading selling pressure |
| 2 | — | Fading MOMO Above Candle | Bear | Range | `AND(ID56.SG1=1, ID13.SG1[-2]>ID13.SG1[-1], ID13.SG1[-1]>ID13.SG1, ID13.SG1>0)` | ID13.SG1 | Delta falling 3 bars but still positive — fading buying pressure |
| 3 | ★ | Delta Rise | Bull | Range | `AND(ID56.SG1=1, ID13.SG1[-3]<ID13.SG1[-2], ID13.SG1[-2]<ID13.SG1[-1], ID13.SG1[-1]<ID13.SG1, ID13.SG1>0)` | ID13.SG1 | Delta rising 4 consecutive bars and positive — sustained buying momentum |
| 4 | ★ | Delta Drop | Bear | Range | `AND(ID56.SG1=1, ID13.SG1[-3]>ID13.SG1[-2], ID13.SG1[-2]>ID13.SG1[-1], ID13.SG1[-1]>ID13.SG1, ID13.SG1<0)` | ID13.SG1 | Delta falling 4 consecutive bars and negative — sustained selling momentum |
| 5 | — | BPOC | Bull | Range | `AND(ID56.SG1=1, C>O, ID22.SG1<=L+(H-L)*0.30, ID22.SG1>=L, …)` *(partial)* | ID22.SG1 | Bullish bar with VPOC in lower 30% of range — price acceptance above VPOC |
| 6 | — | BPOC | Bear | Range | `AND(ID56.SG1=1, C<O, ID22.SG1>=H-(H-L)*0.30, ID22.SG1<=H, …)` *(partial)* | ID22.SG1 | Bearish bar with VPOC in upper 30% of range — price rejection below VPOC |
| 7 | — | EXH | Bull | Range | `AND(AVAP(L,0)=0, BVAP(L,0)<=9, C>O, C[-1]<O[-1], C[-2]<O[-2], C[-3]<O[-3])` | — | 3 bearish bars then bullish close, zero ask vol at low — selling exhaustion reversal |
| 8 | — | EXH | Bear | Range | `AND(AVAP(H,0)<=9, BVAP(H,0)=0, C<O, C[-1]>O[-1], C[-2]>O[-2], C[-3]>O[-3])` | — | 3 bullish bars then bearish close, zero bid vol at high — buying exhaustion reversal |
| 9 | ★ | VOL SEQ + Delta DIV | Bull | Range | `AND(ID56.SG1=1, ID8.SG1<0, C>O, AVAP(L,0)<AVAP(L+TICKSIZE,0), …)` *(partial)* | ID8.SG1 | Positive close, negative delta, ask volume tapering from low — bullish delta divergence with vol sequence |
| 10 | ★ | VOL SEQ + Delta DIV | Bear | Range | `AND(ID56.SG1=1, ID8.SG1>0, C<O, BVAP(H,0)<BVAP(H-TICKSIZE,0), …)` *(partial)* | ID8.SG1 | Negative close, positive delta, bid volume tapering from high — bearish delta divergence with vol sequence |
| 11 | — | VA Long (Value Area Engulfing) | Bull | Range | `AND(ID56.SG1=1, ID42.SG1>ID42.SG1[-1], ID42.SG2<ID42.SG2[-1], C>O, C>C[-1])` | ID42.SG1, ID42.SG2 | VA expanding (VAH rising, VAL falling) with bullish close — value area engulfing long |
| 12 | — | VA Short (Value Area Engulfing) | Bear | Range | `AND(ID56.SG1=1, ID42.SG1>ID42.SG1[-1], ID42.SG2<ID42.SG2[-1], C<O, C<C[-1])` | ID42.SG1, ID42.SG2 | VA expanding (VAH rising, VAL falling) with bearish close — value area engulfing short |
| 13 | ★★ | Delta Slingshot Buy | Bull | Range | `AND(ID4.SG4[-2]<=ID5.SG3, ID4.SG4[-1]<0, C[-1]>O[-1], ID4.SG4>ID5.SG1, C>O, C>C[-1])` | ID4.SG4, ID5.SG1, ID5.SG3 | Delta hit bear extreme 2 bars ago, bullish reversal bar, then delta surges above bull extreme — slingshot long |
| 14 | ★★ | Delta Slingshot Sell | Bear | Range | `AND(ID4.SG4[-2]>=ID5.SG1, ID4.SG4[-1]>0, C[-1]<O[-1], ID4.SG4<ID5.SG3, C<O, C<C[-1])` | ID4.SG4, ID5.SG1, ID5.SG3 | Delta hit bull extreme 2 bars ago, bearish reversal bar, then delta drops below bear extreme — slingshot short |
| 15 | — | POC Delta | Bull | Range | `AND(ID56.SG1=1, ID37.SG1>O, C>O, C>C[-1], C[-1]<O[-1], ID4.SG4>ID5.SG1)` | ID4.SG4, ID5.SG1, ID37.SG1 | Price opens below VPOC, closes above after bearish prior bar, extreme bull delta — VPOC reclaim with delta confirmation |
| 16 | — | POC Delta | Bear | Range | `AND(ID56.SG1=1, ID37.SG1<O, C<O, C<C[-1], C[-1]>O[-1], ID4.SG4<ID5.SG3)` | ID4.SG4, ID5.SG3, ID37.SG1 | Price opens above VPOC, closes below after bullish prior bar, extreme bear delta — VPOC rejection with delta confirmation |
| 17 | ★ | MPOC + NYSE Tick | Bull | Range | `AND(ID56.SG1=1, C>ID37.SG1, O>ID37.SG1, C>C[-1], L<L[-1], AVAP(L,0)=0, ID47.SG4<=-200)` | ID37.SG1, ID47.SG4 | Close and open above VPOC, higher close, lower low with zero ask at low, NYSE Tick ≤ −200 — VPOC hold with extreme negative tick |
| 18 | ★ | MPOC + NYSE Tick | Bear | Range | `AND(ID56.SG1=1, C<ID37.SG1, O<ID37.SG1, C<C[-1], H>H[-1], BVAP(H,0)=0, ID47.SG4>=200)` | ID37.SG1, ID47.SG4 | Close and open below VPOC, lower close, higher high with zero bid at high, NYSE Tick ≥ +200 — VPOC rejection with extreme positive tick |
| 19 | ★★ | Delta Trap (ΔT) | Bull | Range | `AND(ID56.SG1=1, C[-2]<O[-2], ID4.SG4[-2]<ID5.SG3, C[-1]>O[-1], ID4.SG4[-1]>0, C>O, ID4.SG4>0, (ID4.SG4[-1]+ID4.SG4)>ABS(ID4.SG4[-2]), ID55.SG1>ID55.SG1[-1], ID55.SG2>ID55.SG2[-1])` | ID4.SG4, ID5.SG3, ID55.SG1, ID55.SG2 | Bear bar with extreme negative delta, then 2 bullish bars recover more delta than initial dump, VA shifting up — trapped sellers with value area confirmation |
| 20 | ★★ | Delta Trap (ΔT) | Bear | Range | `AND(ID56.SG1=1, C[-2]>O[-2], ID4.SG4[-2]>ID5.SG1, C[-1]<O[-1], ID4.SG4[-1]<0, C<O, ID4.SG4<0, (ID4.SG4[-1]+ID4.SG4)<-ABS(ID4.SG4[-2]), ID55.SG1<ID55.SG1[-1], ID55.SG2<ID55.SG2[-1])` | ID4.SG4, ID5.SG1, ID55.SG1, ID55.SG2 | Bull bar with extreme positive delta, then 2 bearish bars dump more delta than initial surge, VA shifting down — trapped buyers with value area confirmation |
| 21 | ★★ | Continuous POC Long (POCL) | Bull | Range | `AND(ID56.SG1=1, C[-2]<O[-2], C[-1]>O[-1], C>O, ID22.SG1[-1]=ID22.SG1[-2], ID4.SG4[-1]>0, ID4.SG4>0, OR(ID4.SG4[-1]>ID5.SG1[-1],ID4.SG4>ID5.SG1), ID55.SG1>ID55.SG1[-1], ID55.SG2>ID55.SG2[-1])` | ID4.SG4, ID5.SG1, ID22.SG1, ID55.SG1, ID55.SG2 | VPOC stable 2 bars, bear-bull-bull pattern with extreme positive delta, VA expanding upward — POC anchoring while value area shifts up |
| 22 | ★★ | Continuous POC Short (POCS) | Bear | Range | `AND(ID56.SG1=1, C[-2]>O[-2], C[-1]<O[-1], C<O, ID22.SG1[-1]=ID22.SG1[-2], ID4.SG4[-1]<0, ID4.SG4<0, OR(ID4.SG4[-1]<ID5.SG3[-1],ID4.SG4<ID5.SG3), ID55.SG1<ID55.SG1[-1], ID55.SG2<ID55.SG2[-1])` | ID4.SG4, ID5.SG3, ID22.SG1, ID55.SG1, ID55.SG2 | VPOC stable 2 bars, bull-bear-bear pattern with extreme negative delta, VA expanding downward — POC anchoring while value area shifts down |
| 23 | — | BR + Delta DIV | Bull | Range | `AND(ID56.SG1=1, ID8.SG1<0, C>O, BVAP(L,0)>0, …)` *(partial)* | ID8.SG1 | Bullish close, negative delta, bid volume at low — bar ratio divergence with buying absorption |
| 24 | — | BR + Delta DIV | Bear | Range | `AND(ID56.SG1=1, ID8.SG1>0, C<O, AVAP(H,0)>0, …)` *(partial)* | ID8.SG1 | Bearish close, positive delta, ask volume at high — bar ratio divergence with selling absorption |
| 25 | — | Stopping Volume (SVOL) | Bull | Range | `AND(C>O, ID22.SG1>=L, ID22.SG1<=L+(H-L)*0.20, (BVAP(L,0)+BVAP(L+TICKSIZE,0))>avg_10)` *(partial)* | ID22.SG1 | Bullish close, VPOC in bottom 20% of range, above-average bid volume at low — absorption stopping selling |
| 26 | — | Stopping Volume (SVOL) | Bear | Range | `AND(C<O, ID37.SG1>=H-(H-L)*0.20, ID37.SG1<=H, (AVAP(H,0)+AVAP(H-TICKSIZE,0))>avg_10)` *(partial)* | ID37.SG1 | Bearish close, VPOC in top 20% of range, above-average ask volume at high — absorption stopping buying |
| 27 | ★★ | OF Long v1 | Bull | Renko 6t | `AND(ID15.SG1=1, OR(ID23.SG4[-1]<=ID24.SG3[-1],ID23.SG4[-2]<=ID24.SG3[-2]), ID23.SG4>ID24.SG1, C>ID31.SG1[-1], AVAP(L,0)=0)` | ID23.SG4, ID24.SG1, ID24.SG3, ID31.SG1 | Delta hit bear BB extreme in prior bars, now above bull BB extreme, close above prior VPOC, zero ask at low — delta slingshot long |
| 28 | ★★ | OF Long v2 | Bull | Renko 6t | `AND(ID15.SG1=1, OR(ID23.SG4[-1]<=ID24.SG3[-1],ID23.SG4[-2]<=ID24.SG3[-2]), ID23.SG4>ID24.SG1, ID7.SG3>0, ID6.SG1[-1]>0, C>ID31.SG1[-1], AVAP(L,0)=0)` | ID6.SG1, ID7.SG3, ID23.SG4, ID24.SG1, ID24.SG3, ID31.SG1 | Same as v1 + vol-at-price threshold alerts (+300/−300) confirming |
| 29 | ★★ | OF Short v1 | Bear | Renko 6t | `AND(ID15.SG1=1, OR(ID23.SG4[-1]>=ID24.SG1[-1],ID23.SG4[-2]>=ID24.SG1[-2]), ID23.SG4<ID24.SG3, C<ID31.SG1[-1], BVAP(H,0)=0)` | ID23.SG4, ID24.SG1, ID24.SG3, ID31.SG1 | Delta hit bull BB extreme in prior bars, now below bear BB extreme, close below prior VPOC, zero bid at high — delta slingshot short |
| 30 | ★★ | OF Short v2 | Bear | Renko 6t | `AND(ID15.SG1=1, OR(ID23.SG4[-1]>=ID24.SG1[-1],ID23.SG4[-2]>=ID24.SG1[-2]), ID23.SG4<ID24.SG3, ID6.SG3>0, ID7.SG1[-1]>0, C<ID31.SG1[-1], BVAP(H,0)=0)` | ID6.SG3, ID7.SG1, ID23.SG4, ID24.SG1, ID24.SG3, ID31.SG1 | Same as v1 + vol-at-price threshold alerts (+300/−300) confirming |
| 31 | — | FA+ | Bull | Renko 6t | `AND(C>O, C[-1]<=O[-1], H[-2]>H[-1], C>ID31.SG1[-1], AVAP(L,0)=0, OR(ID23.SG4[-1]<=ID24.SG3[-1],ID23.SG4[-2]<=ID24.SG3[-2]), ID23.SG4>=ID24.SG1)` | ID23.SG4, ID24.SG1, ID24.SG3, ID31.SG1 | Bull bar after bearish bar, prior high declining, close above prior VPOC, zero ask at low, delta reversal from bear to bull extreme — failed auction long |
| 32 | — | FA− | Bear | Renko 6t | `AND(C<O, C[-1]>=O[-1], L[-2]<L[-1], C<ID31.SG1[-1], BVAP(H,0)=0, OR(ID23.SG4[-1]>=ID24.SG1[-1],ID23.SG4[-2]>=ID24.SG1[-2]), ID23.SG4<=ID24.SG3)` | ID23.SG4, ID24.SG1, ID24.SG3, ID31.SG1 | Bear bar after bullish bar, prior low rising, close below prior VPOC, zero bid at high, delta reversal from bull to bear extreme — failed auction short |
| 33 | ★★ | Long Trigger | Bull | Renko 8t | `AND(ID5.SG4>ID5.SG1, ID11.SG1<ID11.SG4, ID21.SG1>ID22.SG1, ID13.SG1>20, C>ID19.SG1[-1], ID39.SG1<-60)` | ID5.SG1, ID5.SG4, ID11.SG1, ID11.SG4, ID13.SG1, ID19.SG1, ID21.SG1, ID22.SG1, ID39.SG1 | MACD bullish cross, SMI structure, 50 EMA above 200 EMA, ADX trending, close above prior VPOC, SMI extreme oversold — multi-confluence trend long |
| 34 | ★★ | Short Trigger | Bear | Renko 8t | `AND(ID5.SG4<ID5.SG1, ID11.SG1>ID11.SG3, ID21.SG1<ID22.SG1, ID13.SG1>20, C<ID19.SG1[-1], ID39.SG1>60)` | ID5.SG1, ID5.SG4, ID11.SG1, ID11.SG3, ID13.SG1, ID19.SG1, ID21.SG1, ID22.SG1, ID39.SG1 | MACD bearish cross, SMI structure, 50 EMA below 200 EMA, ADX trending, close below prior VPOC, SMI extreme overbought — multi-confluence trend short |
| 35 | — | T BUY | Bull | Renko 8t | `AND(ID13.SG1>25, ID26.SG1<20, CROSSFROMBELOW(ID26.SG1,ID26.SG2), V>1.5*((V[-1]+V[-2]+V[-3]+V[-4]+V[-5])/5), ID42.SG4>ID42.SG4[-1], ID39.SG1<-60)` | ID13.SG1, ID26.SG1, ID26.SG2, ID39.SG1, ID42.SG4 | Strong ADX, Stochastic crossing up from oversold, vol spike 1.5× 5-bar avg, cumulative delta rising, SMI extreme oversold — trend momentum buy |
| 36 | — | T SELL | Bear | Renko 8t | `AND(ID13.SG1>25, ID26.SG1>80, CROSSFROMABOVE(ID26.SG1,ID26.SG2), V>1.5*((V[-1]+V[-2]+V[-3]+V[-4]+V[-5])/5), ID42.SG4<ID42.SG4[-1], ID39.SG1>60)` | ID13.SG1, ID26.SG1, ID26.SG2, ID39.SG1, ID42.SG4 | Strong ADX, Stochastic crossing down from overbought, vol spike 1.5× 5-bar avg, cumulative delta falling, SMI extreme overbought — trend momentum sell |
| 37 | — | R BUY | Bull | Renko 8t | `AND(ID13.SG1<20, ID31.SG1<30, C<=ID35.SG3, BV>3*AV, ID39.SG1<-60)` | ID13.SG1, ID31.SG1, ID35.SG3, ID39.SG1 | ADX below 20 (ranging), RSI oversold, price at/below lower BB, bid vol 3× ask vol, SMI extreme — mean-reversion buy in ranging market |
| 38 | — | R SELL | Bear | Renko 8t | `AND(ID13.SG1<20, ID31.SG1>70, C>=ID35.SG1, AV>3*BV, ID39.SG1>60)` | ID13.SG1, ID31.SG1, ID35.SG1, ID39.SG1 | ADX below 20 (ranging), RSI overbought, price at/above upper BB, ask vol 3× bid vol, SMI extreme — mean-reversion sell in ranging market |

| 39 | ★★ | POC Wave | Bull | Range | `AND(C[-2]<O[-2], C[-1]>O[-1], C>O, ID37.SG1[-1]<ID37.SG1[-2], ID37.SG1>ID37.SG1[-2], AVAP(L,0)<1, BVAP(L,0)<=9)` | ID37.SG1 | Bear-bull-bull pattern, VPOC dips then recovers above 2-bar prior level, zero ask vol + minimal bid vol at low — VPOC wave bottom reversal |
| 40 | ★★ | POC Wave | Bear | Range | `AND(C[-2]>O[-2], C[-1]<O[-1], C<O, ID37.SG1[-1]>ID37.SG1[-2], ID37.SG1<ID37.SG1[-2], BVAP(H,0)<1, AVAP(H,0)<=9)` | ID37.SG1 | Bull-bear-bear pattern, VPOC rises then drops below 2-bar prior level, zero bid vol + minimal ask vol at high — VPOC wave top reversal |

★★ = highest priority (most used) &nbsp;&nbsp; ★ = used &nbsp;&nbsp; — = not in active set

*(partial) = formula captured partially; full version in source Google Doc.*

---

## Study IDs — Range Bar Chart

> ID56 = account/session validation — excluded from signal analysis.

| ID | SGs Used | Study | Params | Appears In | Signal Role |
|----|----------|-------|--------|------------|-------------|
| **ID4** | SG4 | `scsf_AskBidVolumeDifferenceBars` | — | #13–16, 19–22 | Bar delta (ask vol − bid vol), continuous |
| **ID5** | SG1, SG3 | `scsf_BollingerBands` | Length 20, STD 0.9, SMA | #13–16, 19–22 | Dynamic delta thresholds — BB upper (SG1=bull extreme) / BB lower (SG3=bear extreme) |
| **ID8** | SG1 | Numbers Bar Calculated Values | — | #9–10, 23–24 | Numbers bar stats (delta, vol, bar ratio) |
| **ID13** | SG1 | Bar Delta Below Bar (SC built-in) | — | #1–4 | Raw bar delta, continuous — cleanest delta-velocity source |
| **ID22** | SG1 | `scsf_VolumePointOfControlForBars` | — | #5–6, 21–22, 25 | VPOC price level |
| **ID37** | SG1 | `scsf_VolumePointOfControlForBars` | — | #15–18, 26 | VPOC price level (different instance / session scope from ID22) |
| **ID42** | SG1, SG2 | `scsf_ValueAreaForBars` | Default (~70%) | #11–12 | Value Area: SG1=VAH, SG2=VAL |
| **ID47** | SG4 | TICK-NYSE Symbol | — | #17–18 | NYSE Tick value (SG4) |
| **ID55** | SG1, SG2 | `scsf_ValueAreaForBars` | VA 68% | #19–22 | Value Area 68%: SG1=VAH, SG2=VAL (tighter than ID42) |

---

## Study IDs — Renko 6t Chart

> ID15 = account/session validation — excluded from signal analysis.

| ID | SGs Used | Study | Params | Appears In | Signal Role |
|----|----------|-------|--------|------------|-------------|
| **ID23** | SG4 | `scsf_AskBidVolumeDifferenceBars` | — | #27–32 | Bar delta — same study as ID4 on Range |
| **ID24** | SG1, SG3 | `scsf_BollingerBands` | Length 20, STD 0.9, SMA | #27–32 | Dynamic delta thresholds — same study as ID5 on Range |
| **ID31** | SG1 | `scsf_VolumePointOfControlForBars` | — | #27–32 | VPOC level — same study as ID22/ID37 on Range |
| **ID6** | SG1, SG3 | `scsf_VolumeAtPriceThresholdAlertV2` | Threshold −300 | #28, 30 | Bearish vol-at-price threshold alert (SG3) |
| **ID7** | SG1, SG3 | `scsf_VolumeAtPriceThresholdAlertV2` | Threshold +300 | #28, 30 | Bullish vol-at-price threshold alert (SG1) |

---

## Study IDs — Renko 8t Chart

| ID | SGs Used | Study | Params | Appears In | Signal Role |
|----|----------|-------|--------|------------|-------------|
| **ID5** | SG1, SG4 | `scsf_MACD` | Fast 9, Slow 26, Signal MA 9 | #33–34 | SG4=MACD line, SG1=Signal — `SG4>SG1` = bullish cross |
| **ID11** | SG1, SG3, SG4 | `scsf_StochasticMomentumIndicator` | OB 40, OS −40, %K 5, %D 3, EMA 3 | #33–34 | SMI lines — Long: `SG1<SG4`, Short: `SG1>SG3` |
| **ID13** | SG1 | `scsf_ADX` | DX 14, MovAvg 14 | #33–38 | **Regime selector**: >25 = strong trend (T), >20 = trend (Long/Short), <20 = ranging (R) |
| **ID19** | SG1 | `scsf_VolumePointOfControlForBars` | — | #33–34 | VPOC price level (1-bar lag) |
| **ID21** | SG1 | `scsf_MovingAverageExponential` | Length 50 | #33–34 | 50 EMA — `ID21>ID22` = uptrend |
| **ID22** | SG1 | `scsf_MovingAverageExponential` | Length 200 | #33–34 | 200 EMA — trend filter reference |
| **ID26** | SG1, SG2 | `scsf_SlowStochastic` | %K 10, Fast %D 3, Slow %D 3, Lines 70/30, SMA | #35–36 | SG1=%K, SG2=%D — crossover from <20 (T BUY) or >80 (T SELL) |
| **ID31** | SG1 | `scsf_RSI` | Length 14, MovAvg 3 | #37–38 | RSI — <30 oversold (R BUY), >70 overbought (R SELL) |
| **ID35** | SG1, SG3 | `scsf_BollingerBands` | Length 20, STD 2 | #37–38 | Price bands — SG3=lower, SG1=upper |
| **ID39** | SG1 | `scsf_StochasticMomentumIndicator` | OB 60, OS −60, %K 5, %D 3, EMA 3 | #33–38 | **Master gate** — `SG1<−60` (all longs) / `SG1>60` (all shorts). Required on every trigger. |
| **ID42** | SG4 | `scsf_CumulativeDeltaBarsVolume` | No rolling, Length 10 | #35–36 | Cumulative delta (10-bar) — rising = T BUY, falling = T SELL |

---

## Architecture Notes

**Three separate study stacks, zero ID overlap:**

| | Range Bar | Renko 6t | Renko 8t |
|--|-----------|----------|----------|
| Delta source | ID4 (`AskBidVolDiff`) | ID23 (same) | ID42 (`CumDeltaBars`) |
| Delta threshold | ID5 (`BB 20/0.9`) | ID24 (same) | — |
| VPOC | ID22, ID37 | ID31 | ID19 |
| Session gate | ID56 | ID15 | ID39 (`SMI ±60`) |
| Regime filter | — | — | ID13 (`ADX`) |
| Trend filter | — | — | ID21/ID22 (`EMA 50/200`) |

**Renko 8t ADX (ID13) splits triggers into two mutually exclusive families:**
- ADX >20/25 → trend triggers (#33–36: Long/Short, T BUY/T SELL)
- ADX <20 → reversal triggers (#37–38: R BUY/R SELL)

**Delta threshold is already adaptive on Range and Renko 6t.** ID5/ID24 are Bollinger Bands (20, 0.9) on delta — the extreme threshold rolls with session volatility. EWMA calibration from the roadmap adds a second adaptive layer on top.

**Missing:** Renko 6t plain triangle triggers (former rows 19–22) — formulas not available.
