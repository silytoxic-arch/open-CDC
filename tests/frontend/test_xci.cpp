// Vivado IP configurations (.xci) as sources: FIFO Generator and AXI4-Stream
// Data FIFO models, in the JSON (Vivado 2020.2+) and XML (IP-XACT) formats.

#include "core/Diagnostic.h"
#include "core/Progress.h"
#include "engine/AnalysisSession.h"
#include "engine/ProjectConfig.h"
#include "frontend/XciModels.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace opencdc;

namespace {

std::size_t count(const engine::AnalysisResult& r, std::string_view code) {
    return static_cast<std::size_t>(
        std::ranges::count_if(r.diagnostics, [&](const Diagnostic& d) { return d.code == code; }));
}

const Diagnostic* find(const engine::AnalysisResult& r, std::string_view code) {
    auto it = std::ranges::find(r.diagnostics, code, &Diagnostic::code);
    return it == r.diagnostics.end() ? nullptr : &*it;
}

std::shared_ptr<const engine::AnalysisResult> run(const engine::ProjectConfig& cfg) {
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    auto r = session.run(cfg, progress, cancel);
    REQUIRE(r);
    for (const auto& d : r->diagnostics)
        UNSCOPED_INFO(format(d));
    return r;
}

std::string param(const std::string& name, const std::string& value) {
    return "\"" + name + "\": [ { \"value\": \"" + value + "\", \"resolve_type\": \"generated\", \"usage\": \"all\" } ]";
}

std::string port(const std::string& name, const char* dir, int left = -1) {
    std::string s = "\"" + name + "\": [ { \"direction\": \"" + dir + "\"";
    if (left >= 0)
        s += ", \"size_left\": \"" + std::to_string(left) + "\", \"size_right\": \"0\"";
    return s + " } ]";
}

// Vivado 2020.2+ JSON: FIFO Generator, independent clocks, 8 x 4096, with
// data counts and an ECC output the model does not cover.
std::string jsonFifo(const std::string& name, bool common = false) {
    std::string ports;
    auto add = [&](const std::string& p) { ports += (ports.empty() ? "" : ",\n        ") + p; };
    add(port("rst", "in"));
    if (common) {
        add(port("clk", "in"));
    }
    else {
        add(port("wr_clk", "in"));
        add(port("rd_clk", "in"));
    }
    add(port("din", "in", 7));
    add(port("wr_en", "in"));
    add(port("rd_en", "in"));
    add(port("dout", "out", 7));
    add(port("full", "out"));
    add(port("empty", "out"));
    if (!common) {
        add(port("rd_data_count", "out", 11));
        add(port("wr_data_count", "out", 11));
    }
    add(port("sbiterr", "out"));
    add(port("wr_rst_busy", "out"));
    add(port("rd_rst_busy", "out"));
    return "{\n  \"schema\": \"xilinx.com:schema:json_instance:1.0\",\n  \"ip_inst\": {\n"
           "    \"xci_name\": \"" + name + "\",\n"
           "    \"component_reference\": \"xilinx.com:ip:fifo_generator:13.2\",\n"
           "    \"ip_revision\": \"5\",\n"
           "    \"parameters\": {\n      \"component_parameters\": {\n        " +
           param("Component_Name", name) + ",\n        " +
           param("Fifo_Implementation", common ? "Common_Clock_Block_RAM" : "Independent_Clocks_Block_RAM") +
           "\n      },\n      \"model_parameters\": {\n        " + param("C_COMMON_CLOCK", common ? "1" : "0") +
           ",\n        " + param("C_INTERFACE_TYPE", "0") + ",\n        " +
           param("C_IMPLEMENTATION_TYPE", common ? "0" : "2") + ",\n        " + param("C_DIN_WIDTH", "8") +
           ",\n        " + param("C_DOUT_WIDTH", "8") + ",\n        " + param("C_WR_DEPTH", "4096") +
           ",\n        " + param("C_RD_DEPTH", "4096") + ",\n        " + param("C_SYNCHRONIZER_STAGE", "3") +
           ",\n        " + param("C_PRELOAD_REGS", "0") + ",\n        " + param("C_PRELOAD_LATENCY", "1") +
           "\n      }\n    },\n"
           "    \"boundary\": {\n      \"ports\": {\n        " +
           ports + "\n      }\n    }\n  }\n}\n";
}

// User logic around the FIFO: written in clk_a, read in clk_b.
std::string fifoTop(const std::string& fifo, bool common = false) {
    return std::string("module top(input clk_a, input clk_b, input rst, input [7:0] d, input we, input re,\n"
                       "           output reg [7:0] q, output reg [11:0] level);\n"
                       "  reg [7:0] din_r; reg we_r;\n"
                       "  always @(posedge clk_a) begin din_r <= d; we_r <= we; end\n"
                       "  wire [7:0] dout; wire full, empty; wire [11:0] rcnt, wcnt;\n  ") +
           fifo + " u_fifo (.rst(rst), " +
           (common ? ".clk(clk_a), " : ".wr_clk(clk_a), .rd_clk(clk_b), ") +
           ".din(din_r), .wr_en(we_r & ~full), .rd_en(re & ~empty),\n"
           "                .dout(dout), .full(full), .empty(empty)" +
           (common ? std::string() : ", .rd_data_count(rcnt), .wr_data_count(wcnt)") +
           ");\n"
           "  always @(posedge clk_b) q <= dout;\n" +
           (common ? std::string() : "  always @(posedge clk_b) level <= rcnt;\n") + "endmodule\n";
}

constexpr const char* kSdc = "create_clock -name a -period 10 [get_ports clk_a]\n"
                             "create_clock -name b -period 7 [get_ports clk_b]\n"
                             "set_clock_groups -asynchronous -group a -group b\n"
                             "set_input_delay -clock a 1 [get_ports {d we}]\n"
                             "set_input_delay -clock b 1 [get_ports re]\n";

engine::ProjectConfig project(const test::TempDir& dir, const std::string& top, std::vector<std::filesystem::path> extra) {
    engine::ProjectConfig cfg;
    cfg.name = "xci";
    cfg.top = "top";
    cfg.sources = {dir.write("top.v", top)};
    for (auto& e : extra)
        cfg.sources.push_back(std::move(e));
    cfg.sourceLibraries.assign(cfg.sources.size(), "");
    cfg.constraints = {dir.write("c.sdc", kSdc)};
    return cfg;
}

} // namespace

TEST_CASE("XCI JSON: an independent-clock FIFO Generator is an asynchronous FIFO", "[frontend][xci]") {
    test::TempDir dir;
    auto r = run(project(dir, fifoTop("fifo_8X4K"), {dir.write("fifo_8X4K.xci", jsonFifo("fifo_8X4K"))}));
    CHECK(count(*r, "FE-UNKNOWN-MODULE") + count(*r, "FE-SLANG-UnknownModule") == 0);
    const auto* info = find(*r, "FE-XCI-MODEL");
    REQUIRE(info);
    CHECK(info->message.find("fifo_8X4K") != std::string::npos);
    CHECK(info->message.find("independent clocks (3-stage synchronizers), 8 x 4096") != std::string::npos);
    // the ECC output has no model
    REQUIRE_FALSE(info->notes.empty());
    CHECK(info->notes.front().find("sbiterr") != std::string::npos);
    REQUIRE(r->cdc);
    CHECK(std::ranges::any_of(r->cdc->structures, [](const auto& s) { return s.type == cdc::SyncType::AsyncFifo; }));
    CHECK(r->cdc->stats.unsynchronized == 0);
    CHECK(r->cdc->stats.synchronized > 0);
}

TEST_CASE("XCI: a common-clock FIFO read in another domain is a crossing", "[frontend][xci]") {
    // negative: the IP model does not hide a crossing the user creates
    test::TempDir dir;
    auto r = run(project(dir, fifoTop("fifo_cc", true), {dir.write("fifo_cc.xci", jsonFifo("fifo_cc", true))}));
    const auto* info = find(*r, "FE-XCI-MODEL");
    REQUIRE(info);
    CHECK(info->message.find("common clock") != std::string::npos);
    REQUIRE(r->cdc);
    CHECK(std::ranges::none_of(r->cdc->structures, [](const auto& s) { return s.type == cdc::SyncType::AsyncFifo; }));
    CHECK(r->cdc->stats.unsynchronized >= 1); // dout (clk_a) -> q (clk_b)
}

TEST_CASE("XCI XML: FIFO Generator ports derived from the model parameters", "[frontend][xci]") {
    // The parameters of Digilent's ZmodADC_SynchonizationFIFO (IP-XACT, Vivado 2019):
    // independent clocks, first-word fall-through, valid and overflow flags.
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<spirit:design xmlns:xilinx=\"http://www.xilinx.com\" "
                      "xmlns:spirit=\"http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009\">\n"
                      "  <spirit:componentInstances><spirit:componentInstance>\n"
                      "    <spirit:instanceName>sync_fifo</spirit:instanceName>\n"
                      "    <spirit:componentRef spirit:vendor=\"xilinx.com\" spirit:library=\"ip\" "
                      "spirit:name=\"fifo_generator\" spirit:version=\"13.2\"/>\n"
                      "    <spirit:configurableElementValues>\n";
    for (const auto& [k, v] : std::vector<std::pair<std::string, std::string>>{
             {"C_COMMON_CLOCK", "0"}, {"C_DIN_WIDTH", "32"}, {"C_DOUT_WIDTH", "32"}, {"C_WR_DEPTH", "16"},
             {"C_RD_DEPTH", "16"}, {"C_IMPLEMENTATION_TYPE", "2"}, {"C_INTERFACE_TYPE", "0"},
             {"C_PRELOAD_LATENCY", "0"}, {"C_PRELOAD_REGS", "1"}, {"C_HAS_RST", "1"}, {"C_HAS_VALID", "1"},
             {"C_HAS_OVERFLOW", "1"}, {"C_HAS_RD_DATA_COUNT", "1"}, {"C_RD_DATA_COUNT_WIDTH", "4"},
             {"C_SYNCHRONIZER_STAGE", "2"}, {"C_EN_SAFETY_CKT", "0"}})
        xml += "      <spirit:configurableElementValue spirit:referenceId=\"MODELPARAM_VALUE." + k + "\">" + v +
               "</spirit:configurableElementValue>\n";
    xml += "      <spirit:configurableElementValue spirit:referenceId=\"PARAM_VALUE.Component_Name\">sync_fifo"
           "</spirit:configurableElementValue>\n"
           "    </spirit:configurableElementValues>\n  </spirit:componentInstance></spirit:componentInstances>\n"
           "</spirit:design>\n";
    auto m = frontend::modelFromXciText(xml);
    REQUIRE(m);
    const auto& v = m.value().verilog;
    CHECK(m.value().module == "sync_fifo");
    CHECK(m.value().summary.find("first-word fall-through") != std::string::npos);
    for (const char* p : {"input  wire rst", "input  wire wr_clk", "input  wire rd_clk", "input  wire [31:0] din",
                          "output wire [31:0] dout", "output wire overflow", "output wire valid",
                          "output wire [3:0] rd_data_count"})
        CHECK(v.find(p) != std::string::npos);
    // negative: ports the configuration does not enable
    for (const char* p : {"wr_rst_busy", "almost_full", "prog_full", "wr_data_count", " srst"})
        CHECK(v.find(p) == std::string::npos);
    // the most significant bits of the 5-bit count (Xilinx data counts narrower than log2(depth)+1)
    CHECK(v.find("assign rd_data_count = oc_rcount[4:1];") != std::string::npos);

    // end to end in a VHDL design, as Digilent instantiates it (component declaration)
    test::TempDir dir;
    engine::ProjectConfig cfg;
    cfg.name = "xci_vhdl";
    cfg.top = "top";
    cfg.sources = {dir.write("top.vhd",
                             "library ieee; use ieee.std_logic_1164.all;\n"
                             "entity top is port (clk_a, clk_b, rst : in std_logic; d : in std_logic_vector(31 downto 0);\n"
                             "  q : out std_logic_vector(31 downto 0)); end entity;\n"
                             "architecture rtl of top is\n"
                             "  component sync_fifo port (rst, wr_clk, rd_clk : in std_logic;\n"
                             "    din : in std_logic_vector(31 downto 0); wr_en, rd_en : in std_logic;\n"
                             "    dout : out std_logic_vector(31 downto 0); full, overflow, empty, valid : out std_logic;\n"
                             "    rd_data_count : out std_logic_vector(3 downto 0)); end component;\n"
                             "  signal dr, dout : std_logic_vector(31 downto 0); signal full, empty, valid : std_logic;\n"
                             "begin\n"
                             "  process (clk_a) begin if rising_edge(clk_a) then dr <= d; end if; end process;\n"
                             "  u : sync_fifo port map (rst => rst, wr_clk => clk_a, rd_clk => clk_b, din => dr,\n"
                             "    wr_en => not full, rd_en => '1', dout => dout, full => full, overflow => open,\n"
                             "    empty => empty, valid => valid, rd_data_count => open);\n"
                             "  process (clk_b) begin if rising_edge(clk_b) then if valid = '1' then q <= dout; end if;"
                             " end if; end process;\n"
                             "end architecture;\n"),
                   dir.write("sync_fifo.xci", xml)};
    cfg.sourceLibraries = {"", ""};
    cfg.constraints = {dir.write("c.sdc", kSdc)};
    auto r = run(cfg);
    REQUIRE(r->cdc);
    CHECK(count(*r, "FE-XCI-MODEL") == 1);
    CHECK(std::ranges::any_of(r->cdc->structures, [](const auto& s) { return s.type == cdc::SyncType::AsyncFifo; }));
    CHECK(r->cdc->stats.unsynchronized == 0);
}

TEST_CASE("XCI: AXI4-Stream Data FIFO and AXI-Stream FIFO Generator ports", "[frontend][xci]") {
    // Digilent's line_buffer (axis_data_fifo 2.0): TREADY, TDATA, TLAST, TUSER; data counts
    const std::string xml =
        "<spirit:design xmlns:spirit=\"http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009\">\n"
        "<spirit:instanceName>line_buffer</spirit:instanceName>\n"
        "<spirit:componentRef spirit:vendor=\"xilinx.com\" spirit:library=\"ip\" spirit:name=\"axis_data_fifo\" "
        "spirit:version=\"2.0\"/>\n"
        "<spirit:configurableElementValue spirit:referenceId=\"MODELPARAM_VALUE.C_AXIS_SIGNAL_SET\">"
        "0b00000000000000000000000010010011</spirit:configurableElementValue>\n"
        "<spirit:configurableElementValue spirit:referenceId=\"MODELPARAM_VALUE.C_AXIS_TDATA_WIDTH\">40"
        "</spirit:configurableElementValue>\n"
        "<spirit:configurableElementValue spirit:referenceId=\"MODELPARAM_VALUE.C_AXIS_TUSER_WIDTH\">1"
        "</spirit:configurableElementValue>\n"
        "<spirit:configurableElementValue spirit:referenceId=\"MODELPARAM_VALUE.C_FIFO_DEPTH\">2048"
        "</spirit:configurableElementValue>\n"
        "<spirit:configurableElementValue spirit:referenceId=\"MODELPARAM_VALUE.C_IS_ACLK_ASYNC\">1"
        "</spirit:configurableElementValue>\n"
        "<spirit:configurableElementValue spirit:referenceId=\"PARAM_VALUE.HAS_WR_DATA_COUNT\">1"
        "</spirit:configurableElementValue>\n"
        "<spirit:configurableElementValue spirit:referenceId=\"PARAM_VALUE.HAS_RD_DATA_COUNT\">1"
        "</spirit:configurableElementValue>\n"
        "</spirit:design>\n";
    auto m = frontend::modelFromXciText(xml);
    REQUIRE(m);
    const auto& v = m.value().verilog;
    for (const char* p : {"input  wire s_axis_aclk", "input  wire m_axis_aclk", "input  wire s_axis_aresetn",
                          "input  wire [39:0] s_axis_tdata", "input  wire s_axis_tlast", "input  wire [0:0] s_axis_tuser",
                          "output wire s_axis_tready", "output wire [39:0] m_axis_tdata", "output wire m_axis_tlast",
                          "output wire [31:0] axis_wr_data_count", "output wire [31:0] axis_rd_data_count"})
        CHECK(v.find(p) != std::string::npos);
    for (const char* p : {"tkeep", "tstrb", "tid", "tdest", "prog_full"})
        CHECK(v.find(p) == std::string::npos);
    // the stored word: tdata [39:0], tlast [40], tuser [41]
    CHECK(v.find("assign m_axis_tlast = oc_rdata[40:40];") != std::string::npos);
    CHECK(v.find("assign m_axis_tuser = oc_rdata[41:41];") != std::string::npos);
    CHECK(m.value().summary.find("independent clocks") != std::string::npos);

    // FIFO Generator with an AXI4 memory-mapped interface: not modelled
    const std::string axi = "{\"schema\": \"xilinx.com:schema:json_instance:1.0\", \"ip_inst\": {\"xci_name\": \"f\","
                            " \"component_reference\": \"xilinx.com:ip:fifo_generator:13.2\", \"parameters\": "
                            "{\"model_parameters\": {" + param("C_INTERFACE_TYPE", "2") + "}}}}";
    auto mm = frontend::modelFromXciText(axi);
    REQUIRE_FALSE(mm);
    CHECK(mm.error().code == "FE-XCI-UNSUPPORTED");
    // not an IP configuration
    auto bad = frontend::modelFromXciText("<foo/>");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "FE-XCI-PARSE");
}

TEST_CASE("XCI: an IP without a model stays an unknown module", "[frontend][xci]") {
    const std::string clkWiz = "{\"schema\": \"xilinx.com:schema:json_instance:1.0\", \"ip_inst\": {\"xci_name\": "
                               "\"fifo_8X4K\", \"component_reference\": \"xilinx.com:ip:clk_wiz:6.0\"}}";
    test::TempDir dir;
    auto cfg = project(dir, fifoTop("fifo_8X4K"), {dir.write("fifo_8X4K.xci", clkWiz)});
    auto r = run(cfg);
    const auto* w = find(*r, "FE-XCI-UNSUPPORTED");
    REQUIRE(w);
    CHECK(w->severity == Severity::Warning);
    CHECK(w->message.find("clk_wiz") != std::string::npos);
    CHECK_FALSE(r->cdc);
    CHECK(count(*r, "FE-UNKNOWN-MODULE") + count(*r, "FE-SLANG-UnknownModule") == 1);
    // with black boxes, the rest of the design is analyzed
    cfg.options.blackBoxUnknownModules = true;
    auto bb = run(cfg);
    REQUIRE(bb->cdc);
    CHECK(count(*bb, "FE-BLACKBOX-MODULE") == 1);
}

TEST_CASE("XCI: ILA and VIO debug cores", "[frontend][xci]") {
    auto xml = [](const std::string& inst, const std::string& ip,
                  const std::vector<std::pair<std::string, std::string>>& params) {
        std::string s = "<spirit:design xmlns:spirit=\"http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009\">\n"
                        "<spirit:instanceName>" + inst + "</spirit:instanceName>\n"
                        "<spirit:componentRef spirit:vendor=\"xilinx.com\" spirit:library=\"ip\" spirit:name=\"" +
                        ip + "\" spirit:version=\"3.0\"/>\n";
        for (const auto& [k, v] : params)
            s += "<spirit:configurableElementValue spirit:referenceId=\"PARAM_VALUE." + k + "\">" + v +
                 "</spirit:configurableElementValue>\n";
        return s + "</spirit:design>\n";
    };
    // Digilent's ila_rxclk: probe widths in component parameters, trigger output
    auto ila = frontend::modelFromXciText(xml("ila_dbg", "ila",
                                              {{"C_NUM_OF_PROBES", "2"}, {"C_PROBE0_WIDTH", "3"},
                                               {"C_PROBE1_WIDTH", "1"}, {"C_TRIGOUT_EN", "true"},
                                               {"C_MONITOR_TYPE", "Native"}}));
    REQUIRE(ila);
    const auto& iv = ila.value().verilog;
    for (const char* p : {"input  wire clk", "input  wire [2:0] probe0", "input  wire [0:0] probe1",
                          "output wire trig_out", "input  wire trig_out_ack"})
        CHECK(iv.find(p) != std::string::npos);
    CHECK(iv.find("trig_in ") == std::string::npos); // negative: no trigger input
    CHECK(ila.value().summary == "2 probe(s), inputs only");

    auto vio = frontend::modelFromXciText(xml("vio_ctrl", "vio",
                                              {{"C_NUM_PROBE_IN", "1"}, {"C_NUM_PROBE_OUT", "1"},
                                               {"C_PROBE_IN0_WIDTH", "4"}, {"C_PROBE_OUT0_WIDTH", "2"},
                                               {"C_PROBE_OUT0_INIT_VAL", "0x3"}}));
    REQUIRE(vio);
    const auto& vv = vio.value().verilog;
    CHECK(vv.find("output wire [1:0] probe_out0") != std::string::npos);
    CHECK(vv.find("always @(posedge clk) oc_probe_out0 <= 2'd3;") != std::string::npos);

    // end to end: a VIO output used in another clock domain is a crossing;
    // ILA internals in the constraints are noted, not warned
    test::TempDir dir;
    engine::ProjectConfig cfg;
    cfg.name = "dbg";
    cfg.top = "top";
    cfg.sources = {dir.write("top.v", "module top(input clk_a, input clk_b, input [3:0] d, output reg [1:0] q);\n"
                                      "  wire [1:0] ctrl;\n"
                                      "  vio_ctrl u_vio (.clk(clk_a), .probe_in0(d), .probe_out0(ctrl));\n"
                                      "  ila_dbg u_ila (.clk(clk_b), .probe0(d[2:0]), .probe1(d[3]), .trig_out(),"
                                      " .trig_out_ack(1'b0));\n"
                                      "  always @(posedge clk_b) q <= ctrl;\n"
                                      "endmodule\n"),
                   dir.write("vio_ctrl.xci", xml("vio_ctrl", "vio",
                                                 {{"C_NUM_PROBE_IN", "1"}, {"C_NUM_PROBE_OUT", "1"},
                                                  {"C_PROBE_IN0_WIDTH", "4"}, {"C_PROBE_OUT0_WIDTH", "2"}})),
                   dir.write("ila_dbg.xci", xml("ila_dbg", "ila",
                                                {{"C_NUM_OF_PROBES", "2"}, {"C_PROBE0_WIDTH", "3"},
                                                 {"C_PROBE1_WIDTH", "1"}, {"C_TRIGOUT_EN", "true"}}))};
    cfg.sourceLibraries = {"", "", ""};
    cfg.constraints = {dir.write("c.xdc", std::string(kSdc) +
                                              "set_false_path -to [get_cells -hierarchical -filter {NAME =~ "
                                              "\"*ila_core_inst/u_ila_cap_ctrl/u_scnt_cmp_q*\" && IS_SEQUENTIAL}]\n")};
    auto r = run(cfg);
    REQUIRE(r->cdc);
    CHECK(count(*r, "FE-XCI-MODEL") == 2);
    CHECK(r->cdc->stats.unsynchronized >= 1); // ctrl (clk_a) -> q (clk_b)
    CHECK(std::ranges::none_of(r->diagnostics, [](const Diagnostic& d) {
        return d.code == "CON-UNMATCHED" && d.severity == Severity::Warning &&
               d.message.find("ila_core_inst") != std::string::npos;
    }));
    CHECK(std::ranges::any_of(r->diagnostics, [](const Diagnostic& d) {
        return d.code == "CON-UNMATCHED" && d.message.find("debug core") != std::string::npos;
    }));
}
