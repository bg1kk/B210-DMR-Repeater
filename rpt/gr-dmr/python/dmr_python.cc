// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include <gnuradio/dmr/frame_decoder.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

PYBIND11_MODULE(dmr_python, module)
{
    using gr::dmr::frame_decoder;

    py::class_<frame_decoder,
               gr::sync_block,
               gr::block,
               gr::basic_block,
               frame_decoder::sptr>(module, "frame_decoder")
        .def(py::init(&frame_decoder::make),
             py::arg("sample_rate") = 4800.0f,
             py::arg("slot") = 0,
             py::arg("color_code") = -1,
             py::arg("test_mode") = false)
        .def("set_slot", &frame_decoder::set_slot)
        .def("set_color_code", &frame_decoder::set_color_code)
        .def("get_sync_count", &frame_decoder::get_sync_count)
        .def("get_frame_count", &frame_decoder::get_frame_count)
        .def("reset", &frame_decoder::reset)
        .def("set_debug", &frame_decoder::set_debug);
}
