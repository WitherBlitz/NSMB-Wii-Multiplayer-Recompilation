// Native call catalog used by InvokeDirectCpu. Keep project-specific HLE
// address mappings here; abi_bridge.h contains only the generic ABI machinery.
//
// This is a .inc, not a header: it has no includes of its own and references
// CpuContext, KnownTypedNativeCpuCall<> and KnownNativeCpuCall<>, all of which
// are declared above its single mid-file include site in abi_bridge.h. It
// cannot compile standalone and must never be included anywhere else.

extern "C" void nw4r__lyt__detail__DrawQuad_800847c0(CpuContext* ctx);
extern "C" void nw4r__lyt__detail__DrawQuad_80084d20(CpuContext* ctx);

extern "C" void GX__SetVtxDesc_8016d3a4(uint32_t a, uint32_t t);
extern "C" void GX__ClearVtxDesc_8016dc34();
extern "C" void GX__SetVtxAttrFmt_8016dc68(uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr);
extern "C" void GX__SetTexCoordGen2_8016e37c(uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm);
extern "C" void GX__SetNumTexGens_8016e5a4(uint32_t n);
extern "C" void GX__DrawDone_8016eab0();
extern "C" void GX__Begin_8016f0f0(uint32_t t, uint32_t vf, uint32_t nv);
extern "C" void GX__SetLineWidth_8016f314(uint32_t width, uint32_t texOffsets);
extern "C" void GX__SetPointSize_8016f348(uint32_t pointSize, uint32_t texOffsets);
extern "C" void GX__SetCullMode_8016f3b8(uint32_t m);
extern "C" void GX__SetNumChans_8017054c(uint32_t n);
extern "C" void GX__SetChanCtrl_80170570(uint32_t ch, uint32_t en, uint32_t as, uint32_t ms, uint32_t lm, uint32_t df, uint32_t af);
extern "C" void GX__InitTexObj_801707f8(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m);
extern "C" void GX__InitTexObjLOD_80170a4c(uint32_t oa, uint32_t mif, uint32_t maf, float mil, float mal, float lb, uint32_t bc, uint32_t el, uint32_t ma);
extern "C" void GX__InvalidateTexAll_80171110();
extern "C" void GX__LoadTexObj_80170f2c(uint32_t oa, uint32_t tid);
extern "C" void GX__SetNumIndStages_80171b38(uint32_t n);
extern "C" void GX__SetTevDirect_80171b58(uint32_t s);
extern "C" void GX__SetTevColorIn_80171ce0(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern "C" void GX__SetTevAlphaIn_80171d20(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern "C" void GX__SetTevColorOp_80171d60(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_);
extern "C" void GX__SetTevAlphaOp_80171db8(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_);
extern "C" void GX__SetTevColorS10_80171e70(uint32_t id, uint32_t cp);
extern "C" void GX__SetTevKColor_80171ed4(uint32_t id, uint32_t cp);
extern "C" void GX__SetTevKColorSel_80171f30(uint32_t s, uint32_t sel);
extern "C" void GX__SetTevKAlphaSel_80171f80(uint32_t s, uint32_t sel);
extern "C" void GX__SetTevSwapMode_80171fd0(uint32_t s, uint32_t rs, uint32_t ts);
extern "C" void GX__SetTevSwapModeTable_8017200c(uint32_t id, uint32_t r, uint32_t g, uint32_t b, uint32_t a);
extern "C" void GX__SetTevOrder_8017214c(uint32_t s, uint32_t c, uint32_t m, uint32_t col);
extern "C" void GX__SetNumTevStages_801722a8(uint32_t n);
extern "C" void GX__SetBlendMode_8017277c(uint32_t t, uint32_t s, uint32_t d, uint32_t op);
extern "C" void GX__LoadPosMtxImm_8017310c(uint32_t ma, uint32_t id);
extern "C" void GX__SetCurrentMtx_80173214(uint32_t id);

extern "C" void GX__EnableTexOffsets_8016f37c(uint32_t coord, uint32_t lineEnable, uint32_t pointEnable);
extern "C" void GX__GetProjectionv_801730cc(uint32_t pa);
extern "C" void GX__GetViewportv_801733e0(uint32_t oa);
extern "C" void GX__GetVtxAttrFmt_8016e04c(uint32_t vf, uint32_t a, uint32_t cp, uint32_t tp, uint32_t fp);
extern "C" void GX__GetVtxAttrFmtv_8016e2b8(uint32_t vf, uint32_t la);
extern "C" void GX__GetVtxDesc_8016d9f0(uint32_t a, uint32_t tp);
extern "C" void GX__GetVtxDescv_8016dba4(uint32_t la);
extern "C" void GX__InitTexObjFilter_80170b6c(uint32_t oa, uint32_t minFilter, uint32_t magFilter);
extern "C" void GX__InitTexObjTlut_80170b64(uint32_t oa, uint32_t tl);
extern "C" void GX__InitTexObjUserData_80170be8(uint32_t oa, uint32_t userData);
extern "C" void GX__InitTexObjWrapMode_80170b50(uint32_t oa, uint32_t ws, uint32_t wt);
extern "C" void GX__InitTlutObj_80170f80(uint32_t oa, uint32_t da, uint32_t f, uint32_t e);
extern "C" void GX__LoadLightObjImm_80170320(uint32_t la, uint32_t lid);
extern "C" void GX__LoadNrmMtxImm_80173188(uint32_t ma, uint32_t id);
extern "C" void GX__LoadNrmMtxIndx3x3_801731e0(uint32_t mi, uint32_t id);
extern "C" void GX__LoadPosMtxIndx_8017315c(uint32_t mi, uint32_t id);
extern "C" void GX__LoadTexMtxImm_80173234(uint32_t ma, uint32_t id, uint32_t t);
extern "C" void GX__LoadTlut_80170fa8(uint32_t oa, uint32_t tl);
extern "C" void GX__SetAlphaCompare_80172088(uint32_t c0, uint32_t r0, uint32_t op, uint32_t c1, uint32_t r1);
extern "C" void GX__SetAlphaUpdate_801727f8(uint32_t en);
extern "C" void GX__SetArray_8016e1c4(uint32_t a, uint32_t b, uint32_t s);
extern "C" void GX__SetChanAmbColor_8017039c(uint32_t c, uint32_t cp);
extern "C" void GX__SetChanMatColor_80170474(uint32_t c, uint32_t cp);
extern "C" void GX__SetColorUpdate_801727cc(uint32_t en);
extern "C" void GX__SetDstAlpha_8017295c(uint32_t en, uint32_t a);
extern "C" void GX__SetFog_801722cc(uint32_t t, float sz, float ez, float nz, float fz, uint32_t cp);
extern "C" void GX__SetIndTexCoordScale_80171968(uint32_t s, uint32_t ss, uint32_t ts);
extern "C" void GX__SetIndTexMtx_80171814(uint32_t id, uint32_t ma, uint32_t se);
extern "C" void GX__SetIndTexOrder_80171a6c(uint32_t s, uint32_t c, uint32_t m);
extern "C" void GX__SetProjection_8017301c(uint32_t ma, uint32_t pt);
extern "C" void GX__SetProjectionv_80173080(uint32_t pa);
extern "C" void GX__SetScissor_80173430(uint32_t l, uint32_t t, uint32_t w, uint32_t h);
extern "C" void GX__SetTevColor_80171e10(uint32_t id, uint32_t cp);
extern "C" void GX__SetTevIndWarp_80171ba0(uint32_t ts, uint32_t is, uint32_t so, uint32_t rm, uint32_t ms);
extern "C" void GX__SetTevOp_80171c4c(uint32_t s, uint32_t m);
extern "C" void GX__SetTexCoordBias_801711fc(uint32_t c, uint32_t se, uint32_t te);
extern "C" void GX__SetTexCoordScaleManually_80171180(uint32_t c, uint32_t en, uint32_t ss, uint32_t ts);
extern "C" void GX__SetViewportJitter_80173378(float l, float t, float w, float h, float nz, float fz, uint32_t f);
extern "C" void GX__SetViewport_801733b4(float l, float t, float w, float h, float nz, float fz);
extern "C" void GX__SetVtxAttrFmtv_8016de08(uint32_t vf, uint32_t la);
extern "C" void GX__SetVtxDescv_8016d608(uint32_t la);
extern "C" void GX__SetZCompLoc_80172858(uint32_t bt);
extern "C" void GX__SetZMode_80172824(uint32_t ce, uint32_t f, uint32_t ue);
extern "C" void GX__SetZTexture_801720c0(uint32_t op, uint32_t f, uint32_t b);
extern "C" int32_t OS__DisableInterrupts_801a65ac();
extern "C" int32_t OS__EnableInterrupts_801a65c0();
extern "C" int32_t OS__RestoreInterrupts_801a65d4(int32_t level);
extern "C" int32_t Strcmp_HLE_802e1ab4(uint32_t a, uint32_t b);
extern "C" void PSMTXConcat_HLE_801c0540(uint32_t a, uint32_t b, uint32_t ab);
extern "C" void DCInvalidateRange_801a1600(uint32_t addr, uint32_t length);
extern "C" void DCFlushRange_801a162c(uint32_t addr, uint32_t length);
extern "C" void DCStoreRange_801a165c(uint32_t addr, uint32_t length);

#define MKW_KNOWN_TYPED_NATIVE(addr, fn, ...) \
template <> struct KnownTypedNativeCpuCall<addr> { \
    static constexpr bool kAvailable = true; \
    static constexpr bool kLeaf = false; \
    static void Invoke(CpuContext* cpu) { (void)cpu; fn(__VA_ARGS__); } \
}

// A leaf native takes its arguments by value, never sees the CpuContext, and neither runs guest
// code nor lets another guest thread run before it returns. It cannot change r14-r31, so
// InvokeDirectCpu calls it bare: no copy of the 18 nonvolatile registers out and back. Do NOT
// list a native here that can reach the scheduler, a guest callback or the frame services.
#define MKW_KNOWN_LEAF_NATIVE(addr, fn, ...) \
template <> struct KnownTypedNativeCpuCall<addr> { \
    static constexpr bool kAvailable = true; \
    static constexpr bool kLeaf = true; \
    static void Invoke(CpuContext* cpu) { (void)cpu; fn(__VA_ARGS__); } \
}
#define MKW_KNOWN_LEAF_NATIVE_RET(addr, fn, ...) \
template <> struct KnownTypedNativeCpuCall<addr> { \
    static constexpr bool kAvailable = true; \
    static constexpr bool kLeaf = true; \
    static void Invoke(CpuContext* cpu) { cpu->gpr[3] = static_cast<uint32_t>(fn(__VA_ARGS__)); } \
}

MKW_KNOWN_LEAF_NATIVE(0x801C37C0u, GX__SetVtxDesc_8016d3a4, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C4070u, GX__ClearVtxDesc_8016dc34);
MKW_KNOWN_LEAF_NATIVE(0x801C40B0u, GX__SetVtxAttrFmt_8016dc68, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_LEAF_NATIVE(0x801C47D0u, GX__SetTexCoordGen2_8016e37c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_LEAF_NATIVE(0x801C4A20u, GX__SetNumTexGens_8016e5a4, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x801C4EA0u, GX__DrawDone_8016eab0);
MKW_KNOWN_TYPED_NATIVE(0x801C5570u, GX__Begin_8016f0f0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE(0x801C57A0u, GX__SetLineWidth_8016f314, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C57E0u, GX__SetPointSize_8016f348, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C5860u, GX__SetCullMode_8016f3b8, cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C6AC0u, GX__SetNumChans_8017054c, cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C6AF0u, GX__SetChanCtrl_80170570, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9]);
MKW_KNOWN_LEAF_NATIVE(0x801C6D90u, GX__InitTexObj_801707f8, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9], cpu->gpr[10]);
MKW_KNOWN_LEAF_NATIVE(0x801C6FF0u, GX__InitTexObjLOD_80170a4c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_LEAF_NATIVE(0x801C76C0u, GX__InvalidateTexAll_80171110);
MKW_KNOWN_LEAF_NATIVE(0x801C74C0u, GX__LoadTexObj_80170f2c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C8110u, GX__SetNumIndStages_80171b38, cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C8130u, GX__SetTevDirect_80171b58, cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C82F0u, GX__SetTevColorIn_80171ce0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_LEAF_NATIVE(0x801C8330u, GX__SetTevAlphaIn_80171d20, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_LEAF_NATIVE(0x801C8370u, GX__SetTevColorOp_80171d60, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_LEAF_NATIVE(0x801C83D0u, GX__SetTevAlphaOp_80171db8, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_LEAF_NATIVE(0x801C8490u, GX__SetTevColorS10_80171e70, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C8500u, GX__SetTevKColor_80171ed4, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C8560u, GX__SetTevKColorSel_80171f30, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C85B0u, GX__SetTevKAlphaSel_80171f80, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C8600u, GX__SetTevSwapMode_80171fd0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE(0x801C8640u, GX__SetTevSwapModeTable_8017200c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_LEAF_NATIVE(0x801C8790u, GX__SetTevOrder_8017214c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_LEAF_NATIVE(0x801C88F0u, GX__SetNumTevStages_801722a8, cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C8DC0u, GX__SetBlendMode_8017277c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_LEAF_NATIVE(0x801C9940u, GX__LoadPosMtxImm_8017310c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C9A60u, GX__SetCurrentMtx_80173214, cpu->gpr[3]);

MKW_KNOWN_LEAF_NATIVE(0x801C5820u, GX__EnableTexOffsets_8016f37c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE(0x801C9900u, GX__GetProjectionv_801730cc, cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C9C40u, GX__GetViewportv_801733e0, cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C44A0u, GX__GetVtxAttrFmt_8016e04c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_LEAF_NATIVE(0x801C4700u, GX__GetVtxAttrFmtv_8016e2b8, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C3E20u, GX__GetVtxDesc_8016d9f0, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C3FE0u, GX__GetVtxDescv_8016dba4, cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C7130u, GX__InitTexObjFilter_80170b6c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE(0x801C7120u, GX__InitTexObjTlut_80170b64, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C7160u, GX__InitTexObjUserData_80170be8, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C7100u, GX__InitTexObjWrapMode_80170b50, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE(0x801C7520u, GX__InitTlutObj_80170f80, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_LEAF_NATIVE(0x801C6880u, GX__LoadLightObjImm_80170320, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C99C0u, GX__LoadNrmMtxImm_80173188, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C9A20u, GX__LoadNrmMtxIndx3x3_801731e0, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C9990u, GX__LoadPosMtxIndx_8017315c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C9A80u, GX__LoadTexMtxImm_80173234, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE(0x801C7550u, GX__LoadTlut_80170fa8, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C86C0u, GX__SetAlphaCompare_80172088, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_LEAF_NATIVE(0x801C8E40u, GX__SetAlphaUpdate_801727f8, cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C4780u, GX__SetArray_8016e1c4, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE(0x801C6900u, GX__SetChanAmbColor_8017039c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C69E0u, GX__SetChanMatColor_80170474, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C8E10u, GX__SetColorUpdate_801727cc, cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C8FC0u, GX__SetDstAlpha_8017295c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C8920u, GX__SetFog_801722cc, cpu->gpr[3], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), static_cast<float>(cpu->fpr[4].d), cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C7F40u, GX__SetIndTexCoordScale_80171968, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE(0x801C7E00u, GX__SetIndTexMtx_80171814, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE(0x801C8040u, GX__SetIndTexOrder_80171a6c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE(0x801C9840u, GX__SetProjection_8017301c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C98B0u, GX__SetProjectionv_80173080, cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C9C60u, GX__SetScissor_80173430, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_LEAF_NATIVE(0x801C8430u, GX__SetTevColor_80171e10, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C8180u, GX__SetTevIndWarp_80171ba0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_LEAF_NATIVE(0x801C8250u, GX__SetTevOp_80171c4c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C77D0u, GX__SetTexCoordBias_801711fc, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE(0x801C7750u, GX__SetTexCoordScaleManually_80171180, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_LEAF_NATIVE(0x801C9BD0u, GX__SetViewportJitter_80173378, static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), static_cast<float>(cpu->fpr[4].d), static_cast<float>(cpu->fpr[5].d), static_cast<float>(cpu->fpr[6].d), cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C9C10u, GX__SetViewport_801733b4, static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), static_cast<float>(cpu->fpr[4].d), static_cast<float>(cpu->fpr[5].d), static_cast<float>(cpu->fpr[6].d));
MKW_KNOWN_LEAF_NATIVE(0x801C4250u, GX__SetVtxAttrFmtv_8016de08, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C3A30u, GX__SetVtxDescv_8016d608, cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C8EB0u, GX__SetZCompLoc_80172858, cpu->gpr[3]);
MKW_KNOWN_LEAF_NATIVE(0x801C8E70u, GX__SetZMode_80172824, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE(0x801C8700u, GX__SetZTexture_801720c0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE_RET(0x801B1140u, OS__DisableInterrupts_801a65ac);
MKW_KNOWN_LEAF_NATIVE_RET(0x801B1160u, OS__EnableInterrupts_801a65c0);
MKW_KNOWN_LEAF_NATIVE_RET(0x801B1180u, OS__RestoreInterrupts_801a65d4, static_cast<int32_t>(cpu->gpr[3]));
MKW_KNOWN_LEAF_NATIVE_RET(0x802E1AB4u, Strcmp_HLE_802e1ab4, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801C0540u, PSMTXConcat_HLE_801c0540, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_LEAF_NATIVE(0x801AC440u, DCInvalidateRange_801a1600, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801AC470u, DCFlushRange_801a162c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_LEAF_NATIVE(0x801AC4A0u, DCStoreRange_801a165c, cpu->gpr[3], cpu->gpr[4]);

#undef MKW_KNOWN_TYPED_NATIVE
#undef MKW_KNOWN_LEAF_NATIVE
#undef MKW_KNOWN_LEAF_NATIVE_RET

#define MKW_KNOWN_NATIVE_CPU_CALL(addr, fn) \
template <> struct KnownNativeCpuCall<addr> { \
    static constexpr bool kAvailable = true; \
    static constexpr uint32_t kNonvolatileFprWriteMask = 0; \
    static constexpr void (*Entry)(CpuContext*) = &fn; \
}

MKW_KNOWN_NATIVE_CPU_CALL(0x802B6C80u, nw4r__lyt__detail__DrawQuad_800847c0);
MKW_KNOWN_NATIVE_CPU_CALL(0x802B71E0u, nw4r__lyt__detail__DrawQuad_80084d20);

extern "C" void OSLockMutex_HLE_801a7ee4(CpuContext* ctx);
MKW_KNOWN_NATIVE_CPU_CALL(0x801B2E60u, OSLockMutex_HLE_801a7ee4);

extern "C" void OSUnlockMutex_HLE_801a7fc0(CpuContext* ctx);
MKW_KNOWN_NATIVE_CPU_CALL(0x801B2F40u, OSUnlockMutex_HLE_801a7fc0);

extern "C" void NW4R_G3D_RotXYZFIdx_HLE(CpuContext* ctx);
MKW_KNOWN_NATIVE_CPU_CALL(0x802382B0u, NW4R_G3D_RotXYZFIdx_HLE);

extern "C" void NW4R_G3D_GetResNode_HLE(CpuContext* ctx);
MKW_KNOWN_NATIVE_CPU_CALL(0x8023B300u, NW4R_G3D_GetResNode_HLE);

extern "C" void NW4R_G3D_TexPatGetResult_HLE(CpuContext* ctx);
MKW_KNOWN_NATIVE_CPU_CALL(0x80245440u, NW4R_G3D_TexPatGetResult_HLE);

#undef MKW_KNOWN_NATIVE_CPU_CALL
