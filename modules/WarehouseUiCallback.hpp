// Pure callback-order helper shared by warehouse production code and offline tests.
#ifndef H3RNDNEW_WAREHOUSE_UI_CALLBACK_HPP_
#define H3RNDNEW_WAREHOUSE_UI_CALLBACK_HPP_

namespace warehouseui {

// The game's modal loop requires an END_DIALOG result after the native callback
// has returned. Keep the native callback outside SEH at the call site; this
// helper only enforces its exactly-once-before-finalize ordering.
template <typename NativeCallback, typename FinalizeClose>
inline int CallNativeThenFinalize(bool closePending,
                                  NativeCallback&& nativeCallback,
                                  FinalizeClose&& finalizeClose) {
    const int nativeResult = nativeCallback();
    return closePending ? finalizeClose() : nativeResult;
}

} // namespace warehouseui

#endif // H3RNDNEW_WAREHOUSE_UI_CALLBACK_HPP_
