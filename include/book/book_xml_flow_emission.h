/*
    3dslibris - book_xml_flow_emission.h

    Flow emission: text fragment queuing, coalescing, and final rendering
    into the page buffer. Extracted from book_xml_parser.cpp.
*/

#pragma once

struct parsedata_t;
class Text;

// Callbacks into book_xml_parser.cpp for anonymous-namespace functions
// needed by the flow emission pipeline.
struct FlowEmissionFns {
  void (*advance_screen)(parsedata_t *p);
  void (*advance_page_overflow)(parsedata_t *p, int lineheight);
  void (*flush_pending_block)(parsedata_t *p, const char *tag);
};

namespace book_xml_flow_emission {

// Apply any deferred inline style changes to the page buffer and Text state.
void ApplyDeferredStyleSync(parsedata_t *p, Text *ts);

// Emit txt (raw UTF-8, already coalesced) into the page buffer using the full
// flow layout pipeline.
void EmitFlowedFragmentRaw(parsedata_t *p, const char *txt, int txtlen,
                           const FlowEmissionFns &fns);

// Queue txt into the inline-tail coalescing buffer (or emit immediately if
// coalescing is disabled or the buffer is full).
void QueueFlowedFragmentRaw(parsedata_t *p, const char *txt, int txtlen,
                            const FlowEmissionFns &fns);

// Flush the inline-tail buffer then apply any deferred style sync.
void FlushInlineTailAndDeferredStyle(parsedata_t *p, Text *ts,
                                     const FlowEmissionFns &fns);

} // namespace book_xml_flow_emission
