# SierraChart Studies

A collection of Sierra Chart ACSIL (C++) custom studies for ES futures scalping, built around order-flow confluence gated by trend-state filters.

## Studies

| File | Description |
|---|---|
| `OrderflowConfluence.cpp` | 25 bull + 25 bear order-flow triggers with shared or independent scoring modes |
| `OrderflowSignalV2.cpp` | Single-direction engine with 50 weighted triggers and 3 intensity levels (`AutoLoop = 0`) |
| `OrderflowSignalV3.cpp` | Continued evolution of V2 |
| `OTFStateFilter.cpp` | Trend state machine (Higher-Lows / Lower-Highs) producing a +1 / 0 / −1 state subgraph |
| `MTFCloseFilter.cpp` | Multi-timeframe close-breakout state across six charts with a colored status bar |
| `MTFCloseFilter.pine` | TradingView Pine v5 port of MTFCloseFilter |
| `BreadthCompositeOscillator.cpp` | NYSE $VOLD + $ADD composite oscillator with dual Z-score normalization — macro bias filter |
| `TapeReader.cpp` | Tape reading study |
| `ReconTape.cpp` | Reconnaissance tape study |
| `ReconTapeV2.cpp` | Updated ReconTape |
| `BigTradesTape.cpp` | Highlights large trades on the tape |
| `DOMReader.cpp` | DOM (Depth of Market) reader |
| `DOMReaderV2.cpp` | Updated DOMReader |
| `DeltaVelocityProfile.cpp` | Delta velocity profiling |
| `DeltaReversalTrigger.cpp` | Delta reversal trigger detection |
| `AbsorptionGradient.cpp` | Absorption gradient visualization |
| `FlowConviction.cpp` | Order-flow conviction scoring |
| `InterestMap.cpp` | Open interest mapping |
| `LiquidityZones.cpp` | Liquidity zone detection |
| `TrappedTraders.cpp` | Trapped trader detection |
| `AVWAPRotation.cpp` | Anchored VWAP rotation study |
