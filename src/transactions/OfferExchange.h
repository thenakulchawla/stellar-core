#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"
#include <functional>
#include <vector>

// This is a giant reference-and-orientation comment for people who, like
// myself, get constantly mixed up when dealing with order-related code and
// concepts. If you find reasoning about orders and markets and spreads
// totally straightforward, skip this, nothing to see here!
//
//
// An order book can be summarized by a diagram, as shown below. It is often
// visible on the trading interface of an exchange, though sometimes inverted
// left-to-right or drawn horizontally. But the idea is the same.
//
// The diagram is split into two stacks of orders. Each stack is the the set of
// orders related to selling an asset _in a trading pair_ with the other
// asset. In this codebase we refer to the generic two sides of an arbitrary
// asset pair as "wheat" and "sheep" for historical reasons. So at the top of
// the diagram there are the orders of people trying to sell sheep (or,
// equivalently, buy wheat). At the bottom there are the orders of people trying
// to sell wheat (or, equivalently, buy sheep).
//
// (A bit of market terminology: an "order book" normally contains two kinds of
// "order": buying is expressed by "bid" orders, and selling is expressed by
// "ask" orders, or "offers". In this codebase we encode both types of order as
// selling -- converting all bids to asks -- and are therefore a little
// inconsistent about using the words "offer" and "order": we only record a
// single direction of order -- asks a.k.a. offers -- and they both start with
// "o" so it's easy and mostly harmless to use the terms "order" and "offer" as
// synonymous here. But outside this codebase, an "offer" often means an "ask"
// order specifically!)
//
//
//                      Willing to sell more sheep at higher sale price
//                --------------------------------------------------------->
//
//            ^  +----------------------------------------------------------+
//     Higher |  |sell 60 sheep                                             |
//     prices |  |@ 100 wheat:1                                             |
//      for   |  +--------------------------------------+-------------------+
//     sheep  |  |sell 40 sheep                         |
//       =    |  |@ 75 wheat:1                          |
//     better |  +-----------------------+--------------+
//      for   |  |sell 25 sheep          |
//     sheep  |  |@ 50 wheat:1           |
//     seller |  +--------------+--------+
//            |  |sell 16 sheep |                           "ask" for sheep,
//            |  |@ 25 wheat:1  |                 <-------- "bid" for wheat
//               +--------------+
//                 ^
//                 | Spread: gap between cheapest sheep for sale in units of
//                 | wheat and cheapest wheat for sale in units of sheep.
//                 | Equivalently: between "ask" and "bid" for either asset.
//                 v
//               +-----------------------+
//    Higher  |  |sell 25 wheat          |        <-------- "ask" for wheat,
//    prices  |  |@ 0.05 sheep:1 (= 1:20)|                  "bid" for sheep
//     for    |  +-----------------------+--------------+
//    wheat   |  |sell 40 wheat                         |
//      =     |  |@ 0.1 sheep:1 (= 1:10)                |
//    better  |  +--------------------------------------+-------------------+
//     for    |  |sell 60 wheat                                             |
//    wheat   |  |@ 0.5 sheep:1 (= 1:2)                                     |
//    seller  |  +----------------------------------------------------------+
//            v
//                --------------------------------------------------------->
//                      Willing to sell more wheat at higher sale price
//
//
// Looking at the diagram, there are a few orientation things to notice and
// think about (at least in their general shape -- we'll discuss strict rules
// governing them in a while):
//
//   1. Being willing to sell wheat for sheep is _exactly the same_ as being
//      willing to buy sheep for wheat. There are differences once we get into
//      which direction of price movement you'll accept as "better than your
//      order", but in general it's just a question of which unit you declare
//      the price and quantity for, so for uniformity sake, our order book
//      encodes orders on both sides of a trading pair as as _selling_.
//
//   2. Generally more people will be willing to _sell_ more of an asset at
//      higher sale prices. This _makes sense_ intuitively, an embodiment of the
//      notion that "everybody has a price". Not everyone will sell their
//      favourite shoes for $100, but probably everyone will for $1,000 or
//      $10,000. Put another way: higher sale prices for an asset are "better"
//      for people trying to sell it. If you're _selling_ at some price "or
//      better", that means "or higher prices". (Equivalently: "or better" means
//      "or _lower_ price" for _buyers_, but again, we model both sides here as
//      _sellers_). Similarly there is (intuitively) a lowest price at which
//      _anyone_ wants to sell an asset, and likely there aren't _many_ people
//      who want to offer it at that cheapest price. So orders naturally "thin
//      out" toward the center where it wouldn't be especially appealing to
//      bother selling, and "widen out" towards the edges where the prices
//      (should they occur) would be tempting for lots of people to sell at.
//
//   3. At any given moment, the order _book_ (this diagram) contains all the
//      orders that are _not_ matched. Orders recorded in the book are the
//      unmatched _residue_ of orders submitted for trading. In other words,
//      when someone submits a new order, the exchange matching engine will
//      compare it to the orders in the book and _execute_ any part it can,
//      swapping assets from the parties involved and effectively _deleting the
//      intersection_ of the submitted order and the order book from both,
//      writing only the _symmetric difference_ of them back into the book.
//      Orders that match (and should be executed / symmetric-differenced)
//      are also called "crossing orders", and sometimes this is verbed and
//      the entire act of matching and executing is called "crossing" a pair
//      of orders/offers.
//
//   4. Because the set of orders is not perfectly smooth -- there is not
//      necessarily every possible quantity of an asset on offer at every
//      possible price -- deleting the symmetric difference of matching orders
//      is likely to open up a _gap_ between the _remaining_ cheapest offers (if
//      there are any!) in either direction of the trading pair. This gap is the
//      _spread_ in the pair. The size of the spread will vary depending on
//      quantity and variability of prices asked by sellers: a sparse or highly
//      variably-priced set of offers will produce a bigger spread.
//
// So far so good? Ok, now let's talk about the more-detailed semantics of the
// orders we support!
//
//
// Limit orders, exact orders, and price selection
// -----------------------------------------------
//
// Orders/Offers (see terminology note above) submitted to the system are
// actually _limit_ offers, meaning that a new order will match against any
// stored offer in he book offering the seller a _higher_ price for their merch
// as well, just not a _lower_ one. So in the diagram above, for example, if
// someone submits an offer to sell sheep at a "cheap" 15 wheat per sheep price,
// then the cheapest wheat at the top of the wheat-selling stack (that priced at
// 20 wheat per sheep, i.e. paying the sheep-seller _more_) still matches the
// new offer and sells, even though the sheep-seller was willing to sell sheep
// for less. An offer for a higher price was already present, and the limit
// order on the sheep sale only bounds the price below (and amount above). It
// will take the higher price too, better for the seller!
//
// What price is the sale made at? The price of the offer stored in the book. In
// fact, offers _in the book_ are not limit offers, they are _exact_ offers that
// strictly cross _at the price written on them_, never anything else. In the
// previous example, the exchange would occur at a rate of 0.05 sheep per wheat
// (or 20 wheat per sheep), because that price is in the exact-priced
// offer-to-sell in the book, not the newly submitted limit offer that's
// crossing it.
//
// The logic for this is in the loop `convertWithOffers`, in which _existing_
// offers are repeatedly loaded from the database and then passed to
// `crossOfferV10`, which calls `exchangeV10` to actually transfer assets, using
// the price in the crossed (stored) offer. There's a lot more (very well
// commented!) complexity in `exchangeV10` to ensure fairness of rounding and
// calculating the symmetric difference, but the price choice is the concern to
// understand here.
//
//
// Alternating price advantages / use of limits
// --------------------------------------------
//
// Returning to our example and going one step further: imagine that
// newly-submitted offer (posted against the diagram) was trying to sell a
// _higher quantity_ than it can sell at _its_ limit price -- say that sheep
// seller wanted to sell 2 sheep at 15 wheat per sheep. Again, that's 0.0666
// sheep per wheat so it's going to cross the cheapest offer selling wheat at
// only 0.05 sheep per wheat. But 2 sheep covers more wheat than is for sale at
// either price: it's enough to buy 30 wheat at 0.0666 and _40_ wheat at
// 0.05. But only 25 is for sale at any price less than 0.1. So the 25 wheat for
// sale will be exchanged (at 0.05 per sheep) for 1.25 sheep, and the remaining
// 0.75 sheep for sale will be written into the order book at 0.0666. In this
// way, the "price advantage" we gave the sheep-seller when crossing -- giving
// them the use of a limit order -- will be reversed in the subsequent exchange,
// if the wheat seller returns to the market with more wheat to sell: they can
// put their own limit order in at 0.05 and still take the better 0.0666 sheep
// per wheat price that the residual 0.75 sheep are waiting to sell at.
//
//
// "Buying" too much, and CAP-0006
// -------------------------------
//
// Returning to our example one more time: imagine that the sheep-seller was
// really more interested in acquiring a specific quantity of wheat than they
// were in selling sheep. They have lots of sheep. But they want to "buy" (say)
// exactly 10 wheat. Can they do this? Our system only deals in sell offers, so
// they convert their interest to "buy" 10 wheat into a sell offer at a number
// that reflects their last accurate knowledge of the market, let's say 0.0666
// sheep per wheat, and offer to sell 0.666 sheep at that price (to get their 10
// wheat, which is what they _want_).
//
// Unfortunately that then crosses with the wheat-seller's offer of 25 wheat @
// 0.05 sheep per wheat and gets them more wheat than they wanted! They get 12
// wheat, and the transaction used up all the sheep they were selling; whereas
// they _wanted_ the matching engine to see the 25 wheat at 0.05 and sell them
// the desired 10 wheat for 0.5 sheep. This phenomenon -- of not being able to
// bound the amount you "buy" because you have to express it as a "sell" -- is
// what motivates CAP-0006. Specifically it happens because crossing attempts to
// sell _all_ of the quantity for sale (offer.amount = 0.6 sheep) rather than as
// little as necessary (at the crossing price) to get the _implicit_ desired
// amount of the _other_ asset (offer.amount / offer.price = 10 wheat).
//
// CAP-0006 therefore adds a new _operation_ type that posts a real honest to
// goodness _buy_ offer, and attempts to cross it. There are two possible
// outcomes: if it is executable in full, then the exchange occurs and the
// crossed offer in the order book is decreased appropriately. If it is _not_
// executable in full, then _part_ of the "buy" is executed (as much as can be)
// and the remainder is converted to a sell offer and stored in the order book,
// as would have been the case before CAP-0006. The reason this is ok is that
// this residue is by definition _not crossing_ with anything in the book -- the
// crossing part just got annihilated -- so (based on the "price selection"
// criteria above) it is setting the _exact_ price at which it will cross in the
// future. When someone else crosses it in the future, they will not buy more of
// what it's selling than it actually wants to sell.
//
//
// Final orientation note
// ----------------------
//
// All the code in the OfferExchange is written from the perspective of
// submitting a bid to buy wheat for sheep, which is equivalent to an offer to
// sell sheep for wheat: the top stack in the diagram above. The offers it
// _queries_ in the database and potentially _crosses_ are those that are
// offering to sell wheat: the bottom stack in the diagram above. This is
// reflected in variable names: numWheatReceived and numSheepSent, for
// example. The sheep are being "sent away from us" and the wheat is being
// "received to us". Us being the sheep-seller.
//
// (Further mnemonic: call stack grows down. We're starting from the top
// sheep-seller perspective and trying to extend an offer that crosses into the
// wheat-seller offers)

namespace stellar
{

class AbstractLedgerTxn;
class ConstLedgerTxnEntry;
class LedgerTxnEntry;
class LedgerTxnHeader;
class TrustLineWrapper;
class ConstTrustLineWrapper;

enum class ExchangeResultType
{
    NORMAL,
    REDUCED_TO_ZERO,
    BOGUS
};

struct ExchangeResult
{
    int64_t numWheatReceived;
    int64_t numSheepSend;
    bool reduced;

    ExchangeResultType
    type() const
    {
        if (numWheatReceived != 0 && numSheepSend != 0)
            return ExchangeResultType::NORMAL;
        else
            return reduced ? ExchangeResultType::REDUCED_TO_ZERO
                           : ExchangeResultType::BOGUS;
    }
};

struct ExchangeResultV10
{
    int64_t numWheatReceived;
    int64_t numSheepSend;
    bool wheatStays;
};

int64_t canSellAtMostBasedOnSheep(LedgerTxnHeader const& header,
                                  Asset const& sheep,
                                  ConstTrustLineWrapper const& sheepLine,
                                  Price const& wheatPrice);

int64_t canSellAtMost(LedgerTxnHeader const& header,
                      LedgerTxnEntry const& account, Asset const& asset,
                      TrustLineWrapper const& trustLine);
int64_t canSellAtMost(LedgerTxnHeader const& header,
                      ConstLedgerTxnEntry const& account, Asset const& asset,
                      ConstTrustLineWrapper const& trustLine);

int64_t canBuyAtMost(LedgerTxnHeader const& header,
                     LedgerTxnEntry const& account, Asset const& asset,
                     TrustLineWrapper const& trustLine);
int64_t canBuyAtMost(LedgerTxnHeader const& header,
                     ConstLedgerTxnEntry const& account, Asset const& asset,
                     ConstTrustLineWrapper const& trustLine);

ExchangeResult exchangeV2(int64_t wheatReceived, Price price,
                          int64_t maxWheatReceive, int64_t maxSheepSend);
ExchangeResult exchangeV3(int64_t wheatReceived, Price price,
                          int64_t maxWheatReceive, int64_t maxSheepSend);
ExchangeResultV10 exchangeV10(Price price, int64_t maxWheatSend,
                              int64_t maxWheatReceive, int64_t maxSheepSend,
                              int64_t maxSheepReceive, bool isPathPayment);

ExchangeResultV10 exchangeV10WithoutPriceErrorThresholds(
    Price price, int64_t maxWheatSend, int64_t maxWheatReceive,
    int64_t maxSheepSend, int64_t maxSheepReceive, bool isPathPayment);
ExchangeResultV10 applyPriceErrorThresholds(Price price, int64_t wheatReceive,
                                            int64_t sheepSend, bool wheatStays,
                                            bool isPathPayment);

int64_t adjustOffer(Price const& price, int64_t maxWheatSend,
                    int64_t maxSheepReceive);

bool checkPriceErrorBound(Price price, int64_t wheatReceive, int64_t sheepSend,
                          bool canFavorWheat);

enum class OfferFilterResult
{
    eKeep,
    eStop
};

enum class ConvertResult
{
    eOK,
    ePartial,
    eFilterStop
};

enum class CrossOfferResult
{
    eOfferPartial,
    eOfferTaken,
    eOfferCantConvert
};

// buys wheat with sheep, crossing as many offers as necessary
ConvertResult convertWithOffers(
    AbstractLedgerTxn& ltx, Asset const& sheep, int64_t maxSheepSent,
    int64_t& sheepSend, Asset const& wheat, int64_t maxWheatReceive,
    int64_t& wheatReceived, bool isPathPayment,
    std::function<OfferFilterResult(LedgerTxnEntry const&)> filter,
    std::vector<ClaimOfferAtom>& offerTrail);
}
