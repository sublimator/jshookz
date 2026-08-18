#include "xdata_qjs_harness.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace {

char const *const kAcct = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
char const *const kDest = "rfkE1aSy9G8Upk4JssnwBxhEv5p4mn2KTy";
char const *const kStObjectDescriptor =
    "STObject views do not support descriptor definitions";
char const *const kStArrayDescriptor =
    "STArray views are fixed-shape and do not support descriptor definitions";
char const *const kStArraySet =
    "STArray views are fixed-shape; only existing elements can be "
    "replaced, and only with objects";
char const *const kStArrayElement =
    "STArray element fields must remain object-valued";
char const *const kStArrayDelete =
    "STArray views are fixed-shape; elements cannot be deleted";

}  // namespace

TEST_F(XdataQjs, DecodePaymentFields)
{
    auto tx = decode_hex(kPaymentHex);
    ASSERT_FALSE(tx.isException()) << exception_text();
    EXPECT_EQ(prop_string(tx.get(), "TransactionType"), "Payment");
    EXPECT_EQ(prop_string(tx.get(), "Account"), kAcct);
    EXPECT_EQ(prop_string(tx.get(), "Destination"), kDest);
    EXPECT_EQ(prop_string(tx.get(), "Amount"), "1000000");
    EXPECT_EQ(prop_string(tx.get(), "Fee"), "10");
    EXPECT_EQ(prop_int(tx.get(), "Sequence"), 1);
    ASSERT_NE(stobject_data(tx.get()), nullptr);
    EXPECT_TRUE(is_stobject(tx.get()));
    jshookz::qjs::OwnedValue plain(ctx, JS_NewObject(ctx));
    EXPECT_FALSE(is_stobject(plain.get()));
}

TEST_F(XdataQjs, NewSTObjectIsInstance)
{
    auto constructed = eval("new STObject()");
    ASSERT_FALSE(constructed.isException()) << exception_text();
    EXPECT_TRUE(is_stobject(constructed.get()));
}

TEST_F(XdataQjs, DecodeAcceptsBytesLike)
{
    auto from_hex = decode_hex(kPaymentHex);
    ASSERT_FALSE(from_hex.isException()) << exception_text();

    auto u8 = uint8_from_hex(kPaymentHex);
    auto from_u8 = call1("decode_object", u8.get());
    ASSERT_FALSE(from_u8.isException()) << exception_text();
    EXPECT_EQ(prop_string(from_u8.get(), "TransactionType"), "Payment");

    size_t offset = 0;
    size_t byte_len = 0;
    auto ab = jshookz::qjs::OwnedValue(
        ctx, JS_GetTypedArrayBuffer(ctx, u8.get(), &offset, &byte_len, nullptr));
    ASSERT_FALSE(ab.isException()) << exception_text();
    auto from_ab = call1("decode_object", ab.get());
    ASSERT_FALSE(from_ab.isException()) << exception_text();
    EXPECT_EQ(prop_string(from_ab.get(), "TransactionType"), "Payment");

    auto arr = eval(
        "(() => { var hex = '" + std::string(kPaymentHex) +
        "'; var a = []; for (var i = 0; i < hex.length; i += 2) "
        "a.push(parseInt(hex.substr(i, 2), 16)); return a; })()");
    auto from_arr = call1("decode_object", arr.get());
    ASSERT_FALSE(from_arr.isException()) << exception_text();
    EXPECT_EQ(prop_string(from_arr.get(), "TransactionType"), "Payment");
}

TEST_F(XdataQjs, SourceMutationAfterDecodeDoesNotAffectView)
{
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(jshookz::qjs::hexDecode(kPaymentHex, bytes));
    auto u8 = jshookz::qjs::OwnedValue(ctx, jshookz::qjs::uint8Array(ctx, bytes));
    auto tx = call1("decode_object", u8.get());
    ASSERT_FALSE(tx.isException()) << exception_text();

    size_t offset = 0;
    size_t byte_len = 0;
    auto buf = jshookz::qjs::OwnedValue(
        ctx, JS_GetTypedArrayBuffer(ctx, u8.get(), &offset, &byte_len, nullptr));
    ASSERT_FALSE(buf.isException()) << exception_text();
    size_t buf_size = 0;
    auto *data = JS_GetArrayBuffer(ctx, &buf_size, buf.get());
    ASSERT_NE(data, nullptr);
    ASSERT_LE(offset + byte_len, buf_size);
    std::fill(data + offset, data + offset + byte_len, 0);

    EXPECT_EQ(prop_string(tx.get(), "TransactionType"), "Payment");
    EXPECT_EQ(prop_string(tx.get(), "Account"), kAcct);
    EXPECT_EQ(encode_hex(tx.get()), std::string(kPaymentHex));
}

TEST_F(XdataQjs, MemoArrayAccess)
{
    auto tx = decode_hex(kMemoHex);
    ASSERT_FALSE(tx.isException()) << exception_text();
    auto memos = prop(tx.get(), "Memos");
    EXPECT_EQ(prop_int(memos.get(), "length"), 1);
    auto first = jshookz::qjs::element(ctx, memos.get(), 0);
    EXPECT_TRUE(is_stobject(first.get()));
    auto memo = prop(first.get(), "Memo");
    EXPECT_EQ(prop_string(memo.get(), "MemoType"), "746578742F706C61696E");
    EXPECT_EQ(prop_string(memo.get(), "MemoData"), "48656C6C6F");
    EXPECT_EQ(encode_hex(tx.get()), std::string(kMemoHex));
}

TEST_F(XdataQjs, NestedChildSurvivesRootDrop)
{
    auto tx = decode_hex(kMemoHex);
    ASSERT_FALSE(tx.isException()) << exception_text();
    auto memos = prop(tx.get(), "Memos");
    auto first = jshookz::qjs::element(ctx, memos.get(), 0);
    auto memo = prop(first.get(), "Memo");
    auto fee = prop(tx.get(), "Fee");
    tx = jshookz::qjs::OwnedValue(ctx, JS_UNDEFINED);
    JS_RunGC(rt);
    EXPECT_EQ(to_std_string(fee.get()), "12");
    EXPECT_EQ(prop_string(memo.get(), "MemoType"), "746578742F706C61696E");
    EXPECT_EQ(prop_string(memo.get(), "MemoData"), "48656C6C6F");
}

TEST_F(XdataQjs, DecodeChurnUnderGc)
{
    int checksum = 0;
    for (int i = 0; i < 500; ++i) {
        auto tx = decode_hex(kMemoHex);
        ASSERT_FALSE(tx.isException()) << exception_text();
        auto memos = prop(tx.get(), "Memos");
        auto first = jshookz::qjs::element(ctx, memos.get(), 0);
        auto memo = prop(first.get(), "Memo");
        checksum += static_cast<int>(prop_string(memo.get(), "MemoData").size());
        if ((i % 50) == 0)
            JS_RunGC(rt);
    }
    EXPECT_EQ(checksum, 500 * 10);
}

TEST_F(XdataQjs, RoundtripUnmodifiedUsesOriginalBytes)
{
    auto tx = decode_hex(kPaymentHex);
    ASSERT_FALSE(tx.isException()) << exception_text();
    EXPECT_EQ(encode_hex(tx.get()), std::string(kPaymentHex));
}

TEST_F(XdataQjs, ObjectKeysAndJsonShape)
{
    auto tx = decode_hex(kPaymentHex);
    ASSERT_FALSE(tx.isException()) << exception_text();
    auto keys = eval("Object.keys");
    JSValueConst argv[] = {tx.get()};
    auto listed = jshookz::qjs::OwnedValue(
        ctx, JS_Call(ctx, keys.get(), JS_UNDEFINED, 1, argv));
    ASSERT_FALSE(listed.isException()) << exception_text();
    EXPECT_EQ(json_text(listed.get()),
        "[\"TransactionType\",\"Sequence\",\"Amount\",\"Fee\","
        "\"Account\",\"Destination\"]");
    EXPECT_EQ(
        json_text(tx.get()),
        "{\"TransactionType\":\"Payment\",\"Sequence\":1,\"Amount\":\"1000000\","
        "\"Fee\":\"10\",\"Account\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
        "\"Destination\":\"rfkE1aSy9G8Upk4JssnwBxhEv5p4mn2KTy\"}");
}

TEST_F(XdataQjs, PropertyCacheAndMutation)
{
    auto tx = decode_hex(kPaymentHex);
    ASSERT_FALSE(tx.isException()) << exception_text();
    auto a = prop(tx.get(), "TransactionType");
    auto b = prop(tx.get(), "TransactionType");
    EXPECT_EQ(to_std_string(a.get()), "Payment");
    EXPECT_EQ(to_std_string(b.get()), "Payment");
    JS_SetPropertyStr(ctx, tx.get(), "Fee", JS_NewString(ctx, "20"));
    EXPECT_EQ(prop_string(tx.get(), "Fee"), "20");
}

TEST_F(XdataQjs, UnknownTrailingFieldThrows)
{
    auto tx = decode_hex(std::string(kPaymentHex) + "10C80000");
    ASSERT_TRUE(tx.isException());
    EXPECT_NE(exception_text().find("unknown field code"), std::string::npos);
}

TEST_F(XdataQjs, NoncanonicalHeadersThrow)
{
    auto a = decode_hex(std::string(kPaymentHex) + "0001");
    EXPECT_TRUE(a.isException());
    (void)exception_text();
    auto b = decode_hex(std::string(kPaymentHex) + "1005");
    EXPECT_TRUE(b.isException());
    (void)exception_text();
}

TEST_F(XdataQjs, ShortXChainBridgeThrowsOnAccess)
{
    auto obj = decode_hex("011901FF");
    if (obj.isException()) {
        EXPECT_NE(exception_text().find("decode_object failed"), std::string::npos);
        return;
    }
    auto bridge = prop(obj.get(), "XChainBridge");
    ASSERT_TRUE(bridge.isException() || JS_IsException(bridge.get()));
    EXPECT_NE(exception_text().find("decode_object failed"), std::string::npos);
}

TEST_F(XdataQjs, EncodeRejectsCompositeShapeViolations)
{
    auto bad = eval(
        "({Account:'rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh', Memos:[{Memo:1}]})");
    auto r = encode(bad.get());
    ASSERT_TRUE(r.isException());
    EXPECT_NE(exception_text().find("TypeError"), std::string::npos);

    auto iou = eval(
        "({Account:'rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh',"
        " Amount:{value:'1', currency:'USD'}})");
    r = encode(iou.get());
    ASSERT_TRUE(r.isException());
    (void)exception_text();

    auto mpt = eval(
        "({Account:'rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh',"
        " Amount:{value:'5', mpt_issuance_id:'AB'}})");
    r = encode(mpt.get());
    ASSERT_TRUE(r.isException());
    (void)exception_text();
}

TEST_F(XdataQjs, UInt64HexLengthCanonicalized)
{
    auto one = eval("({OwnerNode:'1'})");
    auto full = eval("({OwnerNode:'0000000000000001'})");
    auto padded = eval("({OwnerNode:'000000000000000001'})");
    auto overflow = eval("({OwnerNode:'1FFFFFFFFFFFFFFFF'})");
    auto hex1 = encode_hex(one.get());
    auto hex16 = encode_hex(full.get());
    EXPECT_EQ(hex1, hex16);
    auto r = encode(padded.get());
    EXPECT_TRUE(r.isException());
    (void)exception_text();
    r = encode(overflow.get());
    EXPECT_TRUE(r.isException());
    (void)exception_text();
    auto mpt = eval(
        "({Account:'rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh',"
        "Amount:{value:'0x000000000000000001',"
        "mpt_issuance_id:'000000000000000000000000000000000000000000000000'}})");
    r = encode(mpt.get());
    EXPECT_TRUE(r.isException());
    (void)exception_text();
}

TEST_F(XdataQjs, MutationsEncodeAndStringify)
{
    auto top = decode_hex(kMemoHex);
    JS_SetPropertyStr(ctx, top.get(), "Fee", JS_NewString(ctx, "20"));
    EXPECT_EQ(prop_string(top.get(), "Fee"), "20");
    auto top2 = decode_hex(encode_hex(top.get()));
    EXPECT_EQ(prop_string(top2.get(), "Fee"), "20");

    auto nested = decode_hex(kMemoHex);
    auto memos = prop(nested.get(), "Memos");
    auto first = jshookz::qjs::element(ctx, memos.get(), 0);
    auto memo = prop(first.get(), "Memo");
    JS_SetPropertyStr(ctx, memo.get(), "MemoData", JS_NewString(ctx, "DEADBEEF"));
    auto nested2 = decode_hex(encode_hex(nested.get()));
    auto n2m = prop(
        jshookz::qjs::element(ctx, prop(nested2.get(), "Memos").get(), 0).get(),
        "Memo");
    EXPECT_EQ(prop_string(n2m.get(), "MemoData"), "DEADBEEF");

    auto element = decode_hex(kMemoHex);
    auto ememos = prop(element.get(), "Memos");
    auto replacement = eval(
        "({Memo:{MemoType:'746578742F706C61696E',MemoData:'ABCD'}})");
    ASSERT_FALSE(replacement.isException()) << exception_text();
    ASSERT_EQ(
        JS_SetPropertyUint32(ctx, ememos.get(), 0, replacement.release()), 1);
    auto element2 = decode_hex(encode_hex(element.get()));
    auto e2m = prop(
        jshookz::qjs::element(ctx, prop(element2.get(), "Memos").get(), 0).get(),
        "Memo");
    EXPECT_EQ(prop_string(e2m.get(), "MemoData"), "ABCD");
}

TEST_F(XdataQjs, DeleteAndAddFields)
{
    auto undef = decode_hex(kMemoHex);
    JS_SetPropertyStr(ctx, undef.get(), "Fee", JS_UNDEFINED);
    EXPECT_TRUE(JS_IsUndefined(prop(undef.get(), "Fee").get()));
    EXPECT_FALSE(has_own(parse_json(json_text(undef.get())).get(), "Fee"));
    auto undef2 = decode_hex(encode_hex(undef.get()));
    EXPECT_FALSE(has_own(undef2.get(), "Fee"));

    auto deleted = decode_hex(kMemoHex);
    JSAtom fee = JS_NewAtom(ctx, "Fee");
    EXPECT_EQ(JS_DeleteProperty(ctx, deleted.get(), fee, 0), 1);
    JS_FreeAtom(ctx, fee);
    EXPECT_FALSE(has_own(deleted.get(), "Fee"));
    auto redecoded = decode_hex(encode_hex(deleted.get()));
    EXPECT_FALSE(has_own(redecoded.get(), "Fee"));
    JS_SetPropertyStr(ctx, deleted.get(), "Fee", JS_NewString(ctx, "77"));
    auto readd = decode_hex(encode_hex(deleted.get()));
    EXPECT_EQ(prop_string(readd.get(), "Fee"), "77");

    auto added = decode_hex(kMemoHex);
    JS_SetPropertyStr(ctx, added.get(), "SourceTag", JS_NewInt32(ctx, 123));
    JS_SetPropertyStr(ctx, added.get(), "NonProtocol", JS_NewInt32(ctx, 99));
    auto added_hex = encode_hex(added.get());
    auto added2 = decode_hex(added_hex);
    EXPECT_EQ(prop_int(added2.get(), "SourceTag"), 123);
    EXPECT_FALSE(has_own(added2.get(), "NonProtocol"));
    EXPECT_NE(added_hex.find("230000007B"), std::string::npos);
    EXPECT_LT(added_hex.find("230000007B"), added_hex.find("2400000064"));
}

TEST_F(XdataQjs, NestedViewEncodeDoesNotFastPathRoot)
{
    auto tx = decode_hex(kMemoHex);
    auto memos = prop(tx.get(), "Memos");
    auto first = jshookz::qjs::element(ctx, memos.get(), 0);
    auto memo = prop(first.get(), "Memo");
    EXPECT_EQ(encode_hex(memo.get()), "7C0A746578742F706C61696E7D0548656C6C6F");
    EXPECT_EQ(encode_hex(tx.get()), std::string(kMemoHex));
}

TEST_F(XdataQjs, InvalidMutatedValueReportsPath)
{
    auto tx = decode_hex(kMemoHex);
    JS_SetPropertyStr(ctx, tx.get(), "Fee", JS_NewString(ctx, "bogus"));
    auto r = encode(tx.get());
    ASSERT_TRUE(r.isException());
    auto msg = exception_text();
    EXPECT_NE(msg.find("encode_object failed"), std::string::npos);
    EXPECT_NE(msg.find("Fee"), std::string::npos);
}

TEST_F(XdataQjs, BadHashAndBlobHexReportPath)
{
    auto hash = eval("({PreviousTxnID: '" + std::string(64, 'G') + "'})");
    auto r = encode(hash.get());
    ASSERT_TRUE(r.isException());
    auto msg = exception_text();
    EXPECT_NE(msg.find("PreviousTxnID"), std::string::npos);
    EXPECT_NE(msg.find("hex"), std::string::npos);

    auto blob = eval("({MemoData: 'GG'})");
    r = encode(blob.get());
    ASSERT_TRUE(r.isException());
    msg = exception_text();
    EXPECT_NE(msg.find("MemoData"), std::string::npos);
    EXPECT_NE(msg.find("hex"), std::string::npos);
}

TEST_F(XdataQjs, FixedShapeArraysAndDescriptorsRejected)
{
    auto g = global();
    JS_SetPropertyStr(ctx, g.get(), "hex", JS_NewString(ctx, kMemoHex));
    auto out = eval(R"JS(
        var tx = decode_object(hex);
        var out = {};
        function capture(fn) {
            try { fn(); return {threw: false}; }
            catch (e) { return {threw: true, name: e.name, message: e.message}; }
        }
        out.dp_value = capture(function() {
            Object.defineProperty(tx, "Fee",
                {value: "99", enumerable: true, configurable: true});
        });
        out.dp_accessor = capture(function() {
            Object.defineProperty(tx, "Fee", {get: function() { return "99"; }});
        });
        out.dp_reflect = capture(function() {
            Reflect.defineProperty(tx, "Fee", {value: "99"});
        });
        out.dp_array = capture(function() {
            Object.defineProperty(tx.Memos, "0", {value: null});
        });
        var w = tx.Memos[0];
        out.wrap_dp = capture(function() {
            Object.defineProperty(w, "Memo", {value: null});
        });
        out.length_write = capture(function() { tx.Memos.length = 2; });
        out.append = capture(function() {
            tx.Memos[tx.Memos.length] = {Memo: {MemoData: "ABCD"}};
        });
        out.named_prop = capture(function() { tx.Memos.Extra = 1; });
        out.delete_array = capture(function() { delete tx.Memos[0]; });
        out.empty_element = capture(function() { tx.Memos[0] = {}; });
        out.wrong_element_name = capture(function() {
            tx.Memos[0] = {SignerEntry: {}};
        });
        out.extra_element_key = capture(function() {
            tx.Memos[0] = {Memo: {}, Extra: {}};
        });
        out.delete_element_field = capture(function() { delete w.Memo; });
        out.undefined_element_field = capture(function() { w.Memo = undefined; });
        out.set_undefined = capture(function() { tx.Memos[0] = undefined; });
        out.set_scalar = capture(function() { tx.Memos[0] = 42; });
        out.set_null = capture(function() { tx.Memos[0] = null; });
        out.enc_unchanged = util_hex(encode_object(tx)).toUpperCase() === hex.toUpperCase();
        out.memo_data = tx.Memos[0].Memo.MemoData;
        out;
    )JS");
    ASSERT_FALSE(out.isException()) << exception_text();
    for (char const *key : {"dp_value", "dp_accessor", "dp_reflect", "wrap_dp"}) {
        EXPECT_EQ(prop_string(prop(out.get(), key).get(), "message"),
            kStObjectDescriptor)
            << key;
    }
    EXPECT_EQ(
        prop_string(prop(out.get(), "dp_array").get(), "message"),
        kStArrayDescriptor);
    for (char const *key :
         {"length_write",
          "append",
          "named_prop",
          "empty_element",
          "wrong_element_name",
          "extra_element_key",
          "set_undefined",
          "set_scalar",
          "set_null"}) {
        EXPECT_NE(
            prop_string(prop(out.get(), key).get(), "message").find(kStArraySet),
            std::string::npos)
            << key;
    }
    for (char const *key : {"delete_element_field", "undefined_element_field"}) {
        EXPECT_EQ(
            prop_string(prop(out.get(), key).get(), "message"), kStArrayElement)
            << key;
    }
    EXPECT_EQ(
        prop_string(prop(out.get(), "delete_array").get(), "message"),
        kStArrayDelete);
    EXPECT_TRUE(JS_ToBool(ctx, prop(out.get(), "enc_unchanged").get()));
    EXPECT_EQ(prop_string(out.get(), "memo_data"), "48656C6C6F");
}

TEST_F(XdataQjs, PlainStArrayElementMustEmitOneProtocolField)
{
    auto obj = eval(R"JS(({
        TransactionType: "Payment",
        Flags: 0,
        Sequence: 100,
        Amount: "500000",
        Fee: "12",
        Account: "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
        Destination: "rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe",
        Memos: [{}]
    }))JS");
    ASSERT_FALSE(obj.isException()) << exception_text();
    auto r = encode(obj.get());
    ASSERT_TRUE(r.isException());
    auto msg = exception_text();
    EXPECT_NE(msg.find("Memos[0]"), std::string::npos) << msg;
    EXPECT_NE(msg.find("exactly one protocol field"), std::string::npos) << msg;
}

TEST_F(XdataQjs, ScalarSTObjectTypedFieldRejected)
{
    auto tx = decode_hex(kMemoHex);
    JS_SetPropertyStr(ctx, tx.get(), "Memo", JS_NewInt32(ctx, 1));
    auto r = encode(tx.get());
    ASSERT_TRUE(r.isException());
    auto msg = exception_text();
    EXPECT_NE(msg.find("Memo"), std::string::npos);
    EXPECT_NE(msg.find("must be an object"), std::string::npos);
}

TEST_F(XdataQjs, ProtoAssignmentIsOrdinaryCacheKey)
{
    auto g = global();
    JS_SetPropertyStr(ctx, g.get(), "hex", JS_NewString(ctx, kMemoHex));
    auto out = eval(R"JS(
        var tx = decode_object(hex);
        tx.__proto__ = {SourceTag: 123};
        ({
            source_tag_type: typeof tx.SourceTag,
            has_source_tag: Object.prototype.hasOwnProperty.call(tx, "SourceTag"),
            is_stobject: tx instanceof STObject,
            has_proto_key: Object.prototype.hasOwnProperty.call(tx, "__proto__"),
            last_key: Object.keys(tx).pop(),
            rehex: util_hex(encode_object(tx)).toUpperCase()
        });
    )JS");
    ASSERT_FALSE(out.isException()) << exception_text();
    EXPECT_EQ(prop_string(out.get(), "source_tag_type"), "undefined");
    EXPECT_FALSE(JS_ToBool(ctx, prop(out.get(), "has_source_tag").get()));
    EXPECT_TRUE(JS_ToBool(ctx, prop(out.get(), "is_stobject").get()));
    EXPECT_TRUE(JS_ToBool(ctx, prop(out.get(), "has_proto_key").get()));
    EXPECT_EQ(prop_string(out.get(), "last_key"), "__proto__");
    EXPECT_EQ(prop_string(out.get(), "rehex"), kMemoHex);
}

TEST_F(XdataQjs, MemoJsonIsObjectIndexMap)
{
    auto tx = decode_hex(kMemoHex);
    ASSERT_FALSE(tx.isException()) << exception_text();
    auto memos = prop(tx.get(), "Memos");
    auto first = jshookz::qjs::element(ctx, memos.get(), 0);
    EXPECT_EQ(json_text(memos.get()).substr(0, 1), "{");
    EXPECT_TRUE(has_own(memos.get(), "0"));
    EXPECT_TRUE(has_own(first.get(), "Memo"));
    EXPECT_EQ(
        json_text(tx.get()),
        "{\"TransactionType\":\"Payment\",\"Flags\":0,\"Sequence\":100,"
        "\"Amount\":\"500000\",\"Fee\":\"12\","
        "\"Account\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
        "\"Destination\":\"rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe\","
        "\"Memos\":{\"0\":{\"Memo\":{\"MemoType\":\"746578742F706C61696E\","
        "\"MemoData\":\"48656C6C6F\"}}}}");
}

TEST_F(XdataQjs, EmptyMemosStringifyAsObject)
{
    auto encoded = eval(R"JS(
        encode_object({
            TransactionType: "Payment",
            Sequence: 1,
            Amount: "1000000",
            Fee: "10",
            Account: "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
            Destination: "rfkE1aSy9G8Upk4JssnwBxhEv5p4mn2KTy",
            Memos: []
        })
    )JS");
    ASSERT_FALSE(encoded.isException()) << exception_text();
    auto tx = call1("decode_object", encoded.get());
    ASSERT_FALSE(tx.isException()) << exception_text();
    EXPECT_EQ(prop_int(prop(tx.get(), "Memos").get(), "length"), 0);
    EXPECT_EQ(json_text(prop(tx.get(), "Memos").get()), "{}");
}

TEST_F(XdataQjs, CodecFixturesRoundtrip)
{
    auto raw = read_file(JSHOOKZ_XDATA_QJS_FIXTURE_DIR "/codec-fixtures.json");
    auto root = parse_json(raw);
    ASSERT_FALSE(root.isException()) << exception_text();
    auto list = prop(root.get(), "stobject");
    ASSERT_TRUE(JS_IsArray(ctx, list.get()));
    int64_t n = 0;
    {
        auto len = prop(list.get(), "length");
        ASSERT_EQ(JS_ToInt64(ctx, &n, len.get()), 0);
    }
    ASSERT_GT(n, 0);
    for (int64_t i = 0; i < n; ++i) {
        auto fix = jshookz::qjs::element(ctx, list.get(), static_cast<uint32_t>(i));
        auto name = prop_string(fix.get(), "name");
        auto hex = prop_string(fix.get(), "hex");
        auto obj = decode_hex(hex);
        ASSERT_FALSE(obj.isException()) << name << " " << exception_text();
        EXPECT_EQ(encode_hex(obj.get()), hex) << name;
        auto got = parse_json(json_text(obj.get()));
        if (name == "Payment_with_memos") {
            auto memos = prop(obj.get(), "Memos");
            EXPECT_EQ(json_text(memos.get()).substr(0, 1), "{") << name;
            EXPECT_TRUE(has_own(memos.get(), "0")) << name;
            continue;
        }
        auto expected = prop(fix.get(), "decoded");
        EXPECT_TRUE(json_equal(got.get(), expected.get())) << name;
    }
}

TEST_F(XdataQjs, SleFixturesDecode)
{
    auto raw = read_file(JSHOOKZ_XDATA_QJS_FIXTURE_DIR "/sle-fixtures.json");
    auto list = parse_json(raw);
    ASSERT_FALSE(list.isException()) << exception_text();
    ASSERT_TRUE(JS_IsArray(ctx, list.get()));
    int64_t n = 0;
    ASSERT_EQ(JS_ToInt64(ctx, &n, prop(list.get(), "length").get()), 0);
    int limit = static_cast<int>(std::min<int64_t>(n, 20));
    int passed = 0;
    for (int i = 0; i < limit; ++i) {
        auto fix = jshookz::qjs::element(ctx, list.get(), static_cast<uint32_t>(i));
        auto obj = decode_hex(prop_string(fix.get(), "hex"));
        if (!obj.isException() && stobject_data(obj.get()) != nullptr)
            ++passed;
        else if (obj.isException())
            (void)exception_text();
    }
    EXPECT_EQ(passed, 20);
}
