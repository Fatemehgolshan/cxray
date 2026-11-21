import os

from m5.objects.AbstractMemory import AbstractMemory
from m5.params import *


class CXRayMem(AbstractMemory):
    """Memory whose timing comes from the CXRay CXL library.

    Every read/write reaching this object is one cxl::TraceRequest; the
    response is sent at the completion time CXRay returns.  Select it with
    se.py --mem-type=CXRayMem; the whole memory range is CXL-attached.
    Stock se.py cannot set custom memory parameters, so the defaults come
    from the environment: CXRAY_CONFIG (path to the .cfg) and
    CXRAY_ATOMIC_LATENCY_NS.
    """

    type = "CXRayMem"
    cxx_header = "cxray_mem.hh"
    cxx_class = "gem5::memory::CXRayMem"

    port = ResponsePort("Response port towards the memory-side crossbar")
    cxray_config = Param.String(
        os.environ.get("CXRAY_CONFIG", ""),
        "CXRay .cfg file (key=value); its output_dir and run_tag name the "
        "statistics JSON written at exit",
    )
    atomic_latency_ns = Param.Float(
        float(os.environ.get("CXRAY_ATOMIC_LATENCY_NS", "100")),
        "Fixed latency (ns) of atomic-mode accesses; CXRay is not called",
    )

    def controller(self):
        # No separate MemCtrl: this object is the memory controller.
        return self
