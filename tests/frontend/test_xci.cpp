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
                               "\"fifo_8X4K\", \"component_reference\": \"xilinx.com:ip:mig_7series:4.1\"}}";
    test::TempDir dir;
    auto cfg = project(dir, fifoTop("fifo_8X4K"), {dir.write("fifo_8X4K.xci", clkWiz)});
    auto r = run(cfg);
    const auto* w = find(*r, "FE-XCI-UNSUPPORTED");
    REQUIRE(w);
    CHECK(w->severity == Severity::Warning);
    CHECK(w->message.find("mig_7series") != std::string::npos);
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

namespace {

std::string spirit(const std::string& inst, const std::string& ip, const std::string& version,
                   const std::vector<std::pair<std::string, std::string>>& model,
                   const std::vector<std::pair<std::string, std::string>>& user = {}) {
    std::string s = "<spirit:design xmlns:spirit=\"http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009\">\n"
                    "<spirit:instanceName>" + inst + "</spirit:instanceName>\n"
                    "<spirit:componentRef spirit:vendor=\"xilinx.com\" spirit:library=\"ip\" spirit:name=\"" + ip +
                    "\" spirit:version=\"" + version + "\"/>\n";
    for (const auto& [k, v] : model)
        s += "<spirit:configurableElementValue spirit:referenceId=\"MODELPARAM_VALUE." + k + "\">" + v +
             "</spirit:configurableElementValue>\n";
    for (const auto& [k, v] : user)
        s += "<spirit:configurableElementValue spirit:referenceId=\"PARAM_VALUE." + k + "\">" + v +
             "</spirit:configurableElementValue>\n";
    return s + "</spirit:design>\n";
}

// Digilent Nexys-A7 OOB ClkGen: 100 MHz in, 107.955 MHz and 197.917 MHz out
std::vector<std::pair<std::string, std::string>> clkGenParams() {
    return {{"C_PRIMITIVE", "MMCM"}, {"C_PRIMARY_PORT", "clk_100MHz_i"}, {"C_PRIM_IN_FREQ", "100.000"},
            {"C_PRIM_SOURCE", "Single_ended_clock_capable_pin"}, {"C_CLK_OUT1_PORT", "clk_100MHz_o"},
            {"C_CLK_OUT2_PORT", "clk_200MHz_o"}, {"C_CLKOUT2_USED", "1"}, {"C_CLKOUT3_USED", "0"},
            {"C_CLKOUT1_DRIVES", "BUFG"}, {"C_CLKOUT2_DRIVES", "BUFG"}, {"C_CLKOUT1_OUT_FREQ", "107.955"},
            {"C_CLKOUT2_OUT_FREQ", "197.917"}, {"C_USE_RESET", "1"}, {"C_RESET_PORT", "reset_i"},
            {"C_USE_LOCKED", "1"}, {"C_LOCKED_PORT", "locked_o"}, {"C_FEEDBACK_SOURCE", "FDBK_AUTO"},
            {"C_MMCM_CLKFBOUT_MULT_F", "11.875"}, {"C_MMCM_CLKIN1_PERIOD", "10.000"},
            {"C_MMCM_CLKOUT0_DIVIDE_F", "11.000"}, {"C_MMCM_CLKOUT1_DIVIDE", "6"}, {"C_MMCM_DIVCLK_DIVIDE", "1"}};
}

} // namespace

TEST_CASE("XCI: Clocking Wizard outputs are clocks derived from its input", "[frontend][xci][clk_wiz]") {
    auto m = frontend::modelFromXciText(spirit("ClkGen", "clk_wiz", "6.0", clkGenParams(), {{"RESET_TYPE", "ACTIVE_HIGH"}}));
    REQUIRE(m);
    const auto& v = m.value().verilog;
    for (const char* p : {"input  wire clk_100MHz_i", "input  wire reset_i", "output wire clk_100MHz_o",
                          "output wire clk_200MHz_o", "output wire locked_o", "MMCME2_ADV", ".CLKFBOUT_MULT_F(11.875)",
                          ".CLKOUT0_DIVIDE_F(11.0)", ".CLKOUT1_DIVIDE(6)", ".RST(reset_i)", "wire clk_100MHz_o_ClkGen"})
        CHECK(v.find(p) != std::string::npos);
    CHECK(v.find("clk_out3") == std::string::npos); // negative: unused outputs
    CHECK(m.value().summary.find("clk_100MHz_o 107.955 MHz") != std::string::npos);

    // end to end: the output clocks, Vivado's names usable in constraints
    test::TempDir dir;
    engine::ProjectConfig cfg;
    cfg.name = "clkwiz";
    cfg.top = "top";
    cfg.sources = {dir.write("top.v", "module top(input clk, input rst, input d, output reg q);\n"
                                      "  wire a, b, l;\n"
                                      "  ClkGen u (.clk_100MHz_i(clk), .reset_i(rst), .clk_100MHz_o(a), .clk_200MHz_o(b),"
                                      " .locked_o(l));\n"
                                      "  reg x; always @(posedge a) x <= d; always @(posedge b) q <= x;\n"
                                      "endmodule\n"),
                   dir.write("ClkGen.xci", spirit("ClkGen", "clk_wiz", "6.0", clkGenParams()))};
    cfg.sourceLibraries = {"", ""};
    cfg.constraints = {dir.write("c.xdc", "create_clock -name sys -period 10 [get_ports clk]\n")};
    auto r = run(cfg);
    REQUIRE(r->clocks);
    const auto* c0 = &r->clocks->clocks[0];
    std::size_t found = 0;
    for (const auto& c : r->clocks->clocks) {
        if (std::ranges::find(c.aliases, std::string("clk_100MHz_o_ClkGen")) != c.aliases.end()) {
            REQUIRE(c.periodNs);
            CHECK((*c.periodNs > 9.26 && *c.periodNs < 9.27)); // 107.955 MHz
            ++found;
        }
        if (std::ranges::find(c.aliases, std::string("clk_200MHz_o_ClkGen")) != c.aliases.end()) {
            REQUIRE(c.periodNs);
            CHECK((*c.periodNs > 5.05 && *c.periodNs < 5.06)); // 197.917 MHz
            ++found;
        }
    }
    (void)c0;
    CHECK(found == 2);
    REQUIRE(r->cdc);
    CHECK(r->cdc->stats.paths == 0); // outputs of one MMCM are synchronous

    // Vivado's clock names in set_clock_groups
    engine::ProjectConfig grouped = cfg;
    grouped.constraints = {dir.write("g.xdc", "create_clock -name sys -period 10 [get_ports clk]\n"
                                              "set_clock_groups -asynchronous -group clk_100MHz_o_ClkGen "
                                              "-group clk_200MHz_o_ClkGen\n")};
    auto g = run(grouped);
    REQUIRE(g->cdc);
    CHECK(g->cdc->stats.paths == 1);
    CHECK(std::ranges::none_of(g->diagnostics, [](const Diagnostic& d) { return d.code == "CON-UNMATCHED"; }));
    // negative: a name that is no clock
    engine::ProjectConfig wrong = cfg;
    wrong.constraints = {dir.write("w.xdc", "create_clock -name sys -period 10 [get_ports clk]\n"
                                            "set_clock_groups -asynchronous -group clk_out1_ClkGen -group sys\n")};
    auto w = run(wrong);
    CHECK(std::ranges::any_of(w->diagnostics, [](const Diagnostic& d) {
        return d.code == "CON-UNMATCHED" && d.message.find("clk_out1_ClkGen") != std::string::npos;
    }));
}

TEST_CASE("XCI: Clocking Wizard with a PLL, a differential input and an active-low reset", "[frontend][xci][clk_wiz]") {
    auto m = frontend::modelFromXciText(spirit("pll0", "clk_wiz", "6.0",
                                               {{"C_PRIMITIVE", "PLL"}, {"C_PRIMARY_PORT", "clk_in1"},
                                                {"C_PRIM_IN_FREQ", "200"},
                                                {"C_PRIM_SOURCE", "Differential_clock_capable_pin"},
                                                {"C_CLK_OUT1_PORT", "clk_out1"}, {"C_USE_RESET", "1"},
                                                {"C_RESET_PORT", "resetn"}, {"C_USE_LOCKED", "0"},
                                                {"C_PLL_CLKFBOUT_MULT", "5"}, {"C_PLL_DIVCLK_DIVIDE", "1"},
                                                {"C_PLL_CLKOUT0_DIVIDE", "8"}, {"C_CLKOUT1_DRIVES", "No_buffer"}},
                                               {{"RESET_TYPE", "ACTIVE_LOW"}}));
    REQUIRE(m);
    const auto& v = m.value().verilog;
    for (const char* p : {"input  wire clk_in1_p", "input  wire clk_in1_n", "IBUFDS", "PLLE2_ADV", ".CLKFBOUT_MULT(5)",
                          ".CLKOUT0_DIVIDE(8)", ".RST(~resetn)", ".CLKIN1_PERIOD(5.0)"})
        CHECK(v.find(p) != std::string::npos);
    CHECK(v.find("output wire locked") == std::string::npos); // negative: no locked port
    CHECK(v.find("BUFG") == std::string::npos);   // no output buffer
}

TEST_CASE("XCI: Block Memory Generator ports by memory type; two clocks cross", "[frontend][xci][blk_mem_gen]") {
    // true dual-port RAM, clka / clkb, output register on port B
    auto tdp = [](const std::string& inst, const std::string& type) {
        return spirit(inst, "blk_mem_gen", "8.4",
                      {{"C_MEM_TYPE", type}, {"C_INTERFACE_TYPE", "0"}, {"C_WRITE_WIDTH_A", "16"},
                       {"C_READ_WIDTH_A", "16"}, {"C_WRITE_WIDTH_B", "16"}, {"C_READ_WIDTH_B", "16"},
                       {"C_WRITE_DEPTH_A", "1024"}, {"C_ADDRA_WIDTH", "10"}, {"C_ADDRB_WIDTH", "10"},
                       {"C_WEA_WIDTH", "1"}, {"C_WEB_WIDTH", "1"}, {"C_HAS_ENA", "1"}, {"C_HAS_ENB", "1"},
                       {"C_HAS_MEM_OUTPUT_REGS_B", "1"}});
    };
    auto m = frontend::modelFromXciText(tdp("ram", "2"));
    REQUIRE(m);
    const auto& v = m.value().verilog;
    for (const char* p : {"input  wire clka", "input  wire clkb", "input  wire [0:0] wea", "input  wire [0:0] web",
                          "input  wire [15:0] dina", "input  wire [15:0] dinb", "output wire [15:0] douta",
                          "output wire [15:0] doutb", "assign doutb = oc_b2[15:0];"})
        CHECK(v.find(p) != std::string::npos);
    CHECK(m.value().summary.find("true dual-port RAM") != std::string::npos);
    // single-port ROM: no write port, no port B
    auto rom = frontend::modelFromXciText(tdp("rom", "3"));
    REQUIRE(rom);
    for (const char* p : {"wea", "dina", "clkb", "doutb"})
        CHECK(rom.value().verilog.find(p) == std::string::npos);

    // end to end: written on clk_a, read on clk_b: a crossing through the memory
    test::TempDir dir;
    auto cfg = project(dir,
                       "module top(input clk_a, input clk_b, input [7:0] d, input we, input re, output reg [7:0] q,\n"
                       "           output reg [11:0] level);\n"
                       "  reg [15:0] wd; always @(posedge clk_a) wd <= {d, d};\n"
                       "  wire [15:0] ra, rb;\n"
                       "  ram u (.clka(clk_a), .ena(1'b1), .wea(we), .addra(10'd0), .dina(wd), .douta(ra),\n"
                       "         .clkb(clk_b), .enb(re), .web(1'b0), .addrb(10'd0), .dinb(16'd0), .doutb(rb));\n"
                       "  always @(posedge clk_b) q <= rb[7:0];\n"
                       "endmodule\n",
                       {dir.write("ram.xci", tdp("ram", "2"))});
    auto r = run(cfg);
    REQUIRE(r->cdc);
    CHECK(r->cdc->stats.unsynchronized >= 1);
    // negative: the same memory as a ROM read on clk_b has nothing to cross
    test::TempDir d2;
    auto rcfg = project(d2,
                        "module top(input clk_a, input clk_b, input [7:0] d, input we, input re, output reg [7:0] q,\n"
                        "           output reg [11:0] level);\n"
                        "  wire [15:0] ra;\n"
                        "  rom u (.clka(clk_b), .ena(re), .addra(10'd0), .douta(ra));\n"
                        "  always @(posedge clk_b) q <= ra[7:0];\n"
                        "endmodule\n",
                        {d2.write("rom.xci", tdp("rom", "3"))});
    auto rr = run(rcfg);
    REQUIRE(rr->cdc);
    CHECK(rr->cdc->stats.paths == 0);
}

TEST_CASE("XCI: one-clock datapath IP (CORDIC) and XADC", "[frontend][xci]") {
    // Digilent Nexys-A7 OOB Square_Root
    const std::string sq = spirit("Square_Root", "cordic", "6.0",
                                  {{"C_HAS_ACLK", "1"}, {"C_HAS_ACLKEN", "0"}, {"C_HAS_ARESETN", "0"},
                                   {"C_HAS_S_AXIS_CARTESIAN", "1"}, {"C_HAS_S_AXIS_PHASE", "0"},
                                   {"C_S_AXIS_CARTESIAN_TDATA_WIDTH", "32"}, {"C_M_AXIS_DOUT_TDATA_WIDTH", "16"},
                                   {"C_THROTTLE_SCHEME", "3"}});
    auto m = frontend::modelFromXciText(sq);
    REQUIRE(m);
    const auto& v = m.value().verilog;
    for (const char* p : {"input  wire aclk", "input  wire s_axis_cartesian_tvalid",
                          "input  wire [31:0] s_axis_cartesian_tdata", "output wire m_axis_dout_tvalid",
                          "output wire [15:0] m_axis_dout_tdata"})
        CHECK(v.find(p) != std::string::npos);
    CHECK(v.find("tready") == std::string::npos); // throttle scheme 3: no tready

    test::TempDir dir;
    auto cfg = project(dir,
                       "module top(input clk_a, input clk_b, input [7:0] d, input we, input re, output reg [7:0] q,\n"
                       "           output reg [11:0] level);\n"
                       "  reg [31:0] x; always @(posedge clk_a) x <= {4{d}};\n"
                       "  wire v; wire [15:0] y;\n"
                       "  Square_Root u (.aclk(clk_a), .s_axis_cartesian_tvalid(we), .s_axis_cartesian_tdata(x),\n"
                       "                 .m_axis_dout_tvalid(v), .m_axis_dout_tdata(y));\n"
                       "  always @(posedge clk_a) q <= y[7:0];\n"
                       "  wire [15:0] xdo; wire xrdy;\n"
                       "  XADC u_xadc (.DCLK(clk_b), .DEN(re), .DADDR(7'd0), .DI(16'd0), .DWE(1'b0), .RESET(1'b0),\n"
                       "               .DO(xdo), .DRDY(xrdy));\n"
                       "  always @(posedge clk_b) level <= xdo[11:0];\n"
                       "endmodule\n",
                       {dir.write("Square_Root.xci", sq)});
    auto r = run(cfg);
    REQUIRE(r->cdc);
    // negative: the CORDIC output read on its own clock, the XADC result on DCLK: nothing crosses
    CHECK(r->cdc->stats.paths == 0);
    CHECK(std::ranges::any_of(r->diagnostics, [](const Diagnostic& d) {
        return d.code == "FE-PRIMITIVES" && d.message.find("XADC") != std::string::npos;
    }));
    // positive: the CORDIC output read on another clock crosses
    test::TempDir d2;
    std::string top = "module top(input clk_a, input clk_b, input [7:0] d, input we, input re, output reg [7:0] q,\n"
                      "           output reg [11:0] level);\n"
                      "  reg [31:0] x; always @(posedge clk_a) x <= {4{d}};\n"
                      "  wire v; wire [15:0] y;\n"
                      "  Square_Root u (.aclk(clk_a), .s_axis_cartesian_tvalid(we), .s_axis_cartesian_tdata(x),\n"
                      "                 .m_axis_dout_tvalid(v), .m_axis_dout_tdata(y));\n"
                      "  always @(posedge clk_b) q <= y[7:0];\n"
                      "endmodule\n";
    auto r2 = run(project(d2, top, {d2.write("Square_Root.xci", sq)}));
    REQUIRE(r2->cdc);
    CHECK(r2->cdc->stats.unsynchronized >= 1);
}

namespace {

// Digilent Nexys-A7 OOB "ddr": DDR2 x16, 4:1, native interface, no input
// buffer, active-low reset, temperature from outside (values from its .xci)
std::vector<std::pair<std::string, std::string>> migDdr2Params() {
    return {{"MEM_TYPE", "DDR2"}, {"NoOfControllers", "1"}, {"ECC", "OFF"}, {"USE_AXI", "0"},
            {"FREQ_HZ", "75007501"}, {"SYSCLK_TYPE", "NOBUF"}, {"REFCLK_TYPE", "NONE"}, {"POLARITY", "ACTIVE_LOW"},
            {"TEMP_MON_CONTROL", "EXTERNAL"}, {"UI_EXTRA_CLOCKS", "FALSE"}, {"DDR2_ADDR_WIDTH", "27"},
            {"DDR2_BANK_WIDTH", "3"}, {"DDR2_CKE_WIDTH", "1"}, {"DDR2_CK_WIDTH", "1"}, {"DDR2_CS_WIDTH", "1"},
            {"DDR2_DM_WIDTH", "2"}, {"DDR2_DQS_WIDTH", "2"}, {"DDR2_DQ_WIDTH", "16"}, {"DDR2_ODT_WIDTH", "1"},
            {"DDR2_ROW_WIDTH", "13"}, {"DDR2_USE_CS_PORT", "1"}, {"DDR2_USE_DM_PORT", "1"}, {"DDR2_USE_ODT_PORT", "1"},
            {"DDR2_nCK_PER_CLK", "4"}, {"DDR2_nCS_PER_RANK", "1"},
            // the DDR3 values of an unused memory type are ignored
            {"DDR3_DQ_WIDTH", "8"}, {"DDR3_nCK_PER_CLK", "2"}};
}

// user logic: a register on the system clock feeds the user interface; the
// read data is captured on ui_clk, and on clk_b (an unrelated clock)
std::string migTop(bool readOnB) {
    return std::string("module top(input clk, input clk_b, input rst_n, input [7:0] d, output reg [7:0] q);\n"
                       "  wire ui_clk, ui_rst, calib, rdy, wrdy, rvalid, rend;\n"
                       "  wire [127:0] rdata; wire [15:0] dq; wire [1:0] dqs_p, dqs_n;\n"
                       "  reg [26:0] addr; always @(posedge clk) addr <= {19'd0, d};\n"
                       "  reg rst_q; always @(posedge clk) rst_q <= rst_n;\n"
                       "  ddr u_ddr (.ddr2_dq(dq), .ddr2_dqs_p(dqs_p), .ddr2_dqs_n(dqs_n), .sys_clk_i(clk), .sys_rst(rst_q),\n"
                       "    .app_addr(addr), .app_cmd(3'd1), .app_en(1'b1), .app_wdf_data(128'd0), .app_wdf_end(1'b0),\n"
                       "    .app_wdf_mask(16'd0), .app_wdf_wren(1'b0), .app_rd_data(rdata), .app_rd_data_end(rend),\n"
                       "    .app_rd_data_valid(rvalid), .app_rdy(rdy), .app_wdf_rdy(wrdy), .app_sr_req(1'b0),\n"
                       "    .app_ref_req(1'b0), .app_zq_req(1'b0), .ui_clk(ui_clk), .ui_clk_sync_rst(ui_rst),\n"
                       "    .device_temp_i(12'd0), .init_calib_complete(calib));\n"
                       "  always @(posedge ") +
           (readOnB ? "clk_b" : "ui_clk") +
           ") q <= rdata[7:0];\n"
           "endmodule\n";
}

engine::ProjectConfig migProject(const test::TempDir& dir, bool readOnB, const std::string& xdc,
                                 const std::string& prj = {}) {
    engine::ProjectConfig cfg;
    cfg.name = "mig";
    cfg.top = "top";
    cfg.sources = {dir.write("top.v", migTop(readOnB)),
                   dir.write("ip/ddr/ddr.xci", spirit("ddr", "mig_7series", "4.1", migDdr2Params(),
                                                      {{"XML_INPUT_FILE", "mig_a.prj"}}))};
    if (!prj.empty())
        dir.write("ip/ddr/mig_a.prj", prj);
    cfg.sourceLibraries = {"", ""};
    cfg.constraints = {dir.write("c.xdc", xdc)};
    return cfg;
}

const clock::Clock* clockWithAlias(const engine::AnalysisResult& r, const std::string& alias) {
    for (const auto& c : r.clocks->clocks)
        if (std::ranges::find(c.aliases, alias) != c.aliases.end())
            return &c;
    return nullptr;
}

constexpr const char* kMigXdc = "create_clock -name sys -period 5 [get_ports clk]\n"
                                "create_clock -name b -period 7 [get_ports clk_b]\n"
                                "set_clock_groups -asynchronous -group sys -group b\n";

} // namespace

TEST_CASE("XCI: memory interface (MIG) ports from the configuration", "[frontend][xci][mig]") {
    auto m = frontend::modelFromXciText(spirit("ddr", "mig_7series", "4.1", migDdr2Params()));
    REQUIRE(m);
    const auto& v = m.value().verilog;
    for (const char* p :
         {"inout  wire [15:0] ddr2_dq", "inout  wire [1:0] ddr2_dqs_p", "output wire [12:0] ddr2_addr",
          "output wire [2:0] ddr2_ba", "output wire ddr2_ras_n", "output wire [0:0] ddr2_ck_p", "output wire [1:0] ddr2_dm",
          "input  wire sys_clk_i", "input  wire sys_rst", "input  wire [26:0] app_addr", "input  wire [127:0] app_wdf_data",
          "input  wire [15:0] app_wdf_mask", "output wire [127:0] app_rd_data", "output wire app_rdy",
          "output wire ui_clk", "output wire ui_clk_sync_rst", "input  wire [11:0] device_temp_i",
          "output wire init_calib_complete", "OPENCDC_CLOCK_PERIOD = 13.33199", "rstdiv0_sync_r", "~sys_rst"})
        CHECK(v.find(p) != std::string::npos);
    // negative: no DDR3 reset, no reference clock, no AXI, no extra clocks
    for (const char* p : {"reset_n", "clk_ref", "s_axi", "ui_addn_clk", "mmcm_locked", "ddr3_"})
        CHECK(v.find(p) == std::string::npos);
    CHECK(m.value().summary.find("DDR2 x16, native interface, ui_clk 75.0075 MHz") != std::string::npos);
    CHECK(std::ranges::any_of(m.value().notes, [](const std::string& n) { return n.find("mig_a.prj") != std::string::npos; }));

    // DDR3 with an AXI4 interface, differential system and reference clocks, an extra clock
    auto params = migDdr2Params();
    for (auto& [k, val] : params) {
        if (k == "MEM_TYPE")
            val = "DDR3";
        if (k == "USE_AXI")
            val = "1";
        if (k == "SYSCLK_TYPE")
            val = "DIFFERENTIAL";
        if (k == "REFCLK_TYPE")
            val = "DIFFERENTIAL";
        if (k == "POLARITY")
            val = "ACTIVE_HIGH";
        if (k == "TEMP_MON_CONTROL")
            val = "INTERNAL";
        if (k == "UI_EXTRA_CLOCKS")
            val = "TRUE";
        if (k == "DDR3_DQ_WIDTH")
            val = "32";
    }
    params.push_back({"DDR3_nCK_PER_CLK", "4"});
    params.push_back({"C_S_AXI_ADDR_WIDTH", "30"});
    params.push_back({"C_S_AXI_DATA_WIDTH", "256"});
    params.push_back({"C_S_AXI_ID_WIDTH", "4"});
    params.push_back({"MMCM_CLKOUT0_EN", "TRUE"});
    params.push_back({"MMCM_CLKOUT0_FREQ", "100"});
    auto a = frontend::modelFromXciText(spirit("mig_axi", "mig_7series", "4.1", params));
    REQUIRE(a);
    const auto& va = a.value().verilog;
    for (const char* p : {"inout  wire [31:0] ddr3_dq", "output wire ddr3_reset_n", "input  wire sys_clk_p",
                          "input  wire sys_clk_n", "IBUFDS", "input  wire clk_ref_p", "input  wire [29:0] s_axi_awaddr",
                          "input  wire [255:0] s_axi_wdata", "output wire [3:0] s_axi_bid", "output wire s_axi_rvalid",
                          "output wire mmcm_locked", "input  wire aresetn", "output wire ui_addn_clk_0",
                          "output wire [11:0] device_temp", "OPENCDC_CLOCK_PERIOD = 10.0"})
        CHECK(va.find(p) != std::string::npos);
    for (const char* p : {"app_addr", "app_wdf_data", "~sys_rst", "ddr2_"}) // negative: no native interface
        CHECK(va.find(p) == std::string::npos);
    CHECK(a.value().summary.find("DDR3 x32, AXI4 interface") != std::string::npos);
    CHECK(a.value().summary.find("ui_addn_clk_0 100 MHz") != std::string::npos);

    // not modelled: other memory types, several controllers
    auto qdr = migDdr2Params();
    qdr.front().second = "QDRIIP";
    auto q = frontend::modelFromXciText(spirit("qdr", "mig_7series", "4.1", qdr));
    REQUIRE_FALSE(q);
    CHECK(q.error().code == "FE-XCI-UNSUPPORTED");
    auto two = migDdr2Params();
    two[1].second = "2";
    auto t = frontend::modelFromXciText(spirit("two", "mig_7series", "4.1", two));
    REQUIRE_FALSE(t);
    CHECK(t.error().message.find("more than one controller") != std::string::npos);
}

TEST_CASE("XCI: memory interface ui_clk is related to the system clock", "[frontend][xci][mig]") {
    test::TempDir dir;
    auto r = run(migProject(dir, false, kMigXdc));
    REQUIRE(r->clocks);
    // Vivado's name and the user's net name
    const auto* ui = clockWithAlias(*r, "clk_pll_i");
    REQUIRE(ui);
    CHECK(std::ranges::find(ui->aliases, std::string("ui_clk")) != ui->aliases.end());
    REQUIRE(ui->periodNs);
    CHECK((*ui->periodNs > 13.33 && *ui->periodNs < 13.34)); // 75.0075 MHz from FREQ_HZ
    REQUIRE(r->cdc);
    // negative: the system clock register into the user interface, and the
    // reset, do not cross: ui_clk is derived from sys
    CHECK(r->cdc->stats.paths == 0);
    CHECK(count(*r, "FE-XCI-MODEL") == 1);

    // positive: the read data captured on an unrelated clock crosses
    test::TempDir d2;
    auto rb = run(migProject(d2, true, kMigXdc));
    REQUIRE(rb->cdc);
    CHECK(rb->cdc->stats.unsynchronized >= 1);

    // positive: the clocks declared asynchronous by Vivado's name: the user
    // interface input and the reset (synchronized by rstdiv0_sync_r) cross
    test::TempDir d3;
    auto ra = run(migProject(d3, false, std::string(kMigXdc) + "set_clock_groups -asynchronous -group clk_pll_i -group sys\n"));
    REQUIRE(ra->cdc);
    CHECK(ra->cdc->stats.paths >= 2);
    CHECK(ra->cdc->stats.unsynchronized >= 1);
    CHECK(std::ranges::none_of(ra->diagnostics, [](const Diagnostic& d) { return d.code == "CON-UNMATCHED"; }));
    CHECK(std::ranges::any_of(ra->cdc->structures, [](const auto& s) {
        return s.type == cdc::SyncType::ResetSync && s.name.find("rstdiv0_sync_r") != std::string::npos;
    }));
    // a constraint on the IP's reset synchronizer matches the model
    test::TempDir d4;
    auto rf = run(migProject(d4, false, std::string(kMigXdc) +
                                            "set_false_path -to [get_pins u_ddr/u_ddr_mig/u_ddr2_infrastructure/"
                                            "rstdiv0_sync_r*/PRE]\n"));
    CHECK(std::ranges::none_of(rf->diagnostics, [](const Diagnostic& d) { return d.code == "CON-UNMATCHED"; }));
    // negative: a register the model does not have
    test::TempDir d5;
    auto rw = run(migProject(d5, false, std::string(kMigXdc) +
                                            "set_false_path -to [get_pins u_ddr/u_ddr_mig/u_ddr3_infrastructure/"
                                            "rstdiv0_sync_r*/PRE]\n"));
    CHECK(std::ranges::any_of(rw->diagnostics, [](const Diagnostic& d) {
        return d.code == "CON-UNMATCHED" && d.message.find("u_ddr3_infrastructure") != std::string::npos;
    }));
}

TEST_CASE("XCI: memory interface clocking from its memory configuration (.prj)", "[frontend][xci][mig]") {
    const std::string prj = "<?xml version='1.0' encoding='UTF-8'?>\n<Project NoOfControllers=\"1\" >\n"
                            "  <Controller number=\"0\" >\n    <MemoryDevice>DDR2_SDRAM/Components/MT47H64M16HR-25E</MemoryDevice>\n"
                            "    <TimePeriod>3333</TimePeriod>\n    <PHYRatio>4:1</PHYRatio>\n"
                            "    <InputClkFreq>200</InputClkFreq>\n  </Controller>\n</Project>\n";
    test::TempDir dir;
    auto r = run(migProject(dir, false, kMigXdc, prj));
    REQUIRE(r->clocks);
    const auto* ui = clockWithAlias(*r, "clk_pll_i");
    REQUIRE(ui);
    REQUIRE(ui->periodNs);
    CHECK((*ui->periodNs > 13.33 && *ui->periodNs < 13.34)); // 3.333 ns x 4
    REQUIRE(ui->periodRatio);                                 // derived from the 5 ns input
    CHECK((*ui->periodRatio > 2.666 && *ui->periodRatio < 2.667));
    const auto* model = find(*r, "FE-XCI-MODEL");
    REQUIRE(model);
    CHECK(model->message.find("from input 200 MHz") != std::string::npos);
    CHECK(std::ranges::none_of(model->notes, [](const std::string& n) { return n.find("was not found") != std::string::npos; }));
    CHECK(count(*r, "CON-INPUT-PERIOD") == 0);

    // the system clock is not the configured 200 MHz: ui_clk follows the input
    test::TempDir d2;
    auto slow = run(migProject(d2, false,
                               "create_clock -name sys -period 10 [get_ports clk]\n"
                               "create_clock -name b -period 7 [get_ports clk_b]\n"
                               "set_clock_groups -asynchronous -group sys -group b\n",
                               prj));
    CHECK(count(*slow, "CON-INPUT-PERIOD") == 1);
    const auto* ui2 = clockWithAlias(*slow, "clk_pll_i");
    REQUIRE(ui2);
    REQUIRE(ui2->periodNs);
    CHECK((*ui2->periodNs > 26.66 && *ui2->periodNs < 26.67));
}

namespace {

// IP-XACT component file of an IP instance (<instance>.xml), as Vivado writes it
std::string component(const std::vector<std::tuple<std::string, std::string, int, bool>>& ports) {
    std::string s = "<?xml version=\"1.0\"?>\n<spirit:component xmlns:spirit=\"http://www.spiritconsortium.org/"
                    "XMLSchema/SPIRIT/1685-2009\" xmlns:xilinx=\"http://www.xilinx.com\">\n<spirit:model><spirit:ports>\n";
    for (const auto& [name, dir, left, enabled] : ports) {
        s += "<spirit:port><spirit:name>" + name + "</spirit:name><spirit:wire><spirit:direction>" + dir +
             "</spirit:direction>";
        if (left >= 0)
            s += "<spirit:vector><spirit:left spirit:format=\"long\">" + std::to_string(left) +
                 "</spirit:left><spirit:right spirit:format=\"long\">0</spirit:right></spirit:vector>";
        s += "</spirit:wire>";
        if (!enabled)
            // as Vivado writes it: a '>' inside the dependency expression
            s += "<spirit:vendorExtensions><xilinx:portInfo><xilinx:enablement><xilinx:isEnabled "
                 "xilinx:resolve=\"dependent\" xilinx:dependency=\"spirit:decode(id('C_IS_DUAL')) >0\">false"
                 "</xilinx:isEnabled></xilinx:enablement></xilinx:portInfo>"
                 "</spirit:vendorExtensions>";
        s += "</spirit:port>\n";
    }
    return s + "</spirit:ports></spirit:model>\n</spirit:component>\n";
}

std::vector<std::tuple<std::string, std::string, int, bool>> axiLitePorts() {
    return {{"s_axi_aclk", "in", -1, true},    {"s_axi_aresetn", "in", -1, true}, {"s_axi_awaddr", "in", 8, true},
            {"s_axi_awvalid", "in", -1, true}, {"s_axi_awready", "out", -1, true}, {"s_axi_wdata", "in", 31, true},
            {"s_axi_wvalid", "in", -1, true},  {"s_axi_wready", "out", -1, true},  {"s_axi_rdata", "out", 31, true},
            {"s_axi_rvalid", "out", -1, true}};
}

} // namespace

TEST_CASE("XCI XML: ports from the component file next to the .xci", "[frontend][xci][component]") {
    // AXI GPIO, one 4-bit channel; the second channel and the interrupt disabled
    auto ports = axiLitePorts();
    for (const auto& p : std::vector<std::tuple<std::string, std::string, int, bool>>{
             {"ip2intc_irpt", "out", -1, false}, {"gpio_io_i", "in", 3, true}, {"gpio_io_o", "out", 3, true},
             {"gpio_io_t", "out", 3, true}, {"gpio2_io_i", "in", 31, false}, {"gpio2_io_o", "out", 31, false}})
        ports.push_back(p);
    const std::string xci = spirit("gpio0", "axi_gpio", "2.0", {{"C_GPIO_WIDTH", "4"}, {"C_IS_DUAL", "0"}});
    test::TempDir dir;
    dir.write("ip/gpio0/gpio0.xml", component(ports));
    auto m = frontend::modelFromXciText(xci, dir.path() / "ip/gpio0");
    REQUIRE(m);
    const auto& v = m.value().verilog;
    for (const char* p : {"input  wire s_axi_aclk", "input  wire [8:0] s_axi_awaddr", "input  wire [3:0] gpio_io_i",
                          "output wire [3:0] gpio_io_o", "output wire [31:0] s_axi_rdata",
                          "always @(posedge s_axi_aclk) oc_gpio_io_o"})
        CHECK(v.find(p) != std::string::npos);
    for (const char* p : {"gpio2_io", "ip2intc_irpt"}) // negative: disabled ports
        CHECK(v.find(p) == std::string::npos);
    // negative: without the component file the ports are unknown
    auto none = frontend::modelFromXciText(xci, dir.path() / "elsewhere");
    REQUIRE_FALSE(none);
    CHECK(none.error().code == "FE-XCI-UNSUPPORTED");
    CHECK(none.error().message.find("gpio0.xml") != std::string::npos);
    // negative: an unresolved width is not guessed
    std::string unresolved = component(ports);
    unresolved.replace(unresolved.find("<spirit:left spirit:format=\"long\">8</spirit:left>"), 48,
                       "<spirit:left spirit:format=\"long\">C_W-1</spirit:left>");
    dir.write("ip/u/gpio0.xml", unresolved);
    CHECK_FALSE(frontend::modelFromXciText(xci, dir.path() / "ip/u"));

    // end to end: GPIO outputs are registers of s_axi_aclk; read on another clock they cross
    test::TempDir d2;
    d2.write("ip/gpio0/gpio0.xml", component(ports));
    auto cfg = project(d2,
                       "module top(input clk_a, input clk_b, input [7:0] d, input we, input re, output reg [7:0] q,\n"
                       "           output reg [11:0] level);\n"
                       "  wire [3:0] o, t; wire aw, w; wire [31:0] rd; wire rv;\n"
                       "  gpio0 u (.s_axi_aclk(clk_a), .s_axi_aresetn(1'b1), .s_axi_awaddr(9'd0), .s_axi_awvalid(we),\n"
                       "           .s_axi_awready(aw), .s_axi_wdata({4{d}}), .s_axi_wvalid(we), .s_axi_wready(w),\n"
                       "           .s_axi_rdata(rd), .s_axi_rvalid(rv), .gpio_io_i(d[3:0]), .gpio_io_o(o), .gpio_io_t(t));\n"
                       "  always @(posedge clk_a) q <= {4'd0, o};\n"
                       "  always @(posedge clk_b) level <= {8'd0, t};\n"
                       "endmodule\n",
                       {d2.write("ip/gpio0/gpio0.xci", xci)});
    auto r = run(cfg);
    REQUIRE(r->cdc);
    CHECK(r->cdc->stats.unsynchronized == 1); // gpio_io_t read on clk_b; o on clk_a does not cross
    CHECK(count(*r, "FE-XCI-MODEL") == 1);
}

TEST_CASE("XCI: Slice, Concat and Constant of a block design", "[frontend][xci][component]") {
    auto slice = frontend::modelFromXciText(spirit("s0", "xlslice", "1.0", {{"DIN_FROM", "2"}, {"DIN_TO", "1"}, {"DIN_WIDTH", "4"}}));
    REQUIRE(slice);
    CHECK(slice.value().verilog.find("input  wire [3:0] Din") != std::string::npos);
    CHECK(slice.value().verilog.find("output wire [1:0] Dout") != std::string::npos);
    CHECK(slice.value().verilog.find("assign Dout = Din[2:1];") != std::string::npos);
    auto rev = frontend::modelFromXciText(spirit("s1", "xlslice", "1.0", {{"DIN_FROM", "0"}, {"DIN_TO", "1"}, {"DIN_WIDTH", "2"}}));
    REQUIRE(rev);
    CHECK(rev.value().verilog.find("assign Dout[1] = Din[1];") != std::string::npos);
    CHECK(rev.value().verilog.find("assign Dout[0] = Din[0];") != std::string::npos);
    // negative: a range outside the input
    CHECK_FALSE(frontend::modelFromXciText(spirit("s2", "xlslice", "1.0", {{"DIN_FROM", "4"}, {"DIN_TO", "0"}, {"DIN_WIDTH", "4"}})));

    auto cat = frontend::modelFromXciText(spirit("c0", "xlconcat", "2.1",
                                                 {{"NUM_PORTS", "3"}, {"IN0_WIDTH", "2"}, {"IN1_WIDTH", "1"},
                                                  {"IN2_WIDTH", "1"}, {"IN3_WIDTH", "5"}}));
    REQUIRE(cat);
    const auto& cv = cat.value().verilog;
    for (const char* p : {"input  wire [1:0] In0", "input  wire [0:0] In2", "output wire [3:0] dout",
                          "assign dout = {In2, In1, In0};"})
        CHECK(cv.find(p) != std::string::npos);
    CHECK(cv.find("In3") == std::string::npos); // negative: beyond NUM_PORTS

    auto k = frontend::modelFromXciText(spirit("k0", "xlconstant", "1.1", {{"CONST_VAL", "0x5"}, {"CONST_WIDTH", "3"}}));
    REQUIRE(k);
    CHECK(k.value().verilog.find("assign dout = 3'd5;") != std::string::npos);

    // end to end: a constant and a concatenation do not create domains; a slice of
    // a clk_a register read on clk_b still crosses
    test::TempDir dir;
    auto cfg = project(dir,
                       "module top(input clk_a, input clk_b, input [7:0] d, input we, input re, output reg [7:0] q,\n"
                       "           output reg [11:0] level);\n"
                       "  reg [3:0] a; always @(posedge clk_a) a <= d[3:0];\n"
                       "  wire [1:0] s; wire [2:0] kk; wire [3:0] c;\n"
                       "  s0 us (.Din(a), .Dout(s));\n"
                       "  k0 uk (.dout(kk));\n"
                       "  c0 uc (.In0(s), .In1(kk[0]), .In2(kk[1]), .dout(c));\n"
                       "  always @(posedge clk_a) q <= {4'd0, c};\n"
                       "  always @(posedge clk_b) level <= {10'd0, s};\n"
                       "endmodule\n",
                       {dir.write("s0.xci", spirit("s0", "xlslice", "1.0", {{"DIN_FROM", "2"}, {"DIN_TO", "1"}, {"DIN_WIDTH", "4"}})),
                        dir.write("k0.xci", spirit("k0", "xlconstant", "1.1", {{"CONST_VAL", "0x5"}, {"CONST_WIDTH", "3"}})),
                        dir.write("c0.xci", spirit("c0", "xlconcat", "2.1",
                                                   {{"NUM_PORTS", "3"}, {"IN0_WIDTH", "2"}, {"IN1_WIDTH", "1"},
                                                    {"IN2_WIDTH", "1"}}))});
    auto r = run(cfg);
    REQUIRE(r->cdc);
    CHECK(r->cdc->stats.paths == 1); // a[2:1] read on clk_b
    CHECK(count(*r, "FE-XCI-MODEL") == 3);
}

TEST_CASE("XCI: AXI Quad SPI sides and their asynchronous FIFOs", "[frontend][xci][component]") {
    auto ports = axiLitePorts();
    for (const auto& p : std::vector<std::tuple<std::string, std::string, int, bool>>{
             {"ext_spi_clk", "in", -1, true}, {"io0_i", "in", -1, true}, {"io0_o", "out", -1, true},
             {"io1_i", "in", -1, true}, {"sck_o", "out", -1, true}, {"ss_o", "out", 0, true},
             {"ip2intc_irpt", "out", -1, true}})
        ports.push_back(p);
    auto spiTop = [](const std::string& rd) {
        return "module top(input clk_a, input clk_b, input [7:0] d, input we, input re, output reg [7:0] q,\n"
               "           output reg [11:0] level);\n"
               "  wire aw, w, rv, irq, mosi, sck; wire [31:0] rdata; wire [0:0] ss;\n"
               "  spi0 u (.s_axi_aclk(clk_a), .s_axi_aresetn(1'b1), .s_axi_awaddr(9'd0), .s_axi_awvalid(we),\n"
               "          .s_axi_awready(aw), .s_axi_wdata({4{d}}), .s_axi_wvalid(we), .s_axi_wready(w),\n"
               "          .s_axi_rdata(rdata), .s_axi_rvalid(rv), .ext_spi_clk(clk_b), .io0_i(re), .io0_o(mosi),\n"
               "          .io1_i(re), .sck_o(sck), .ss_o(ss), .ip2intc_irpt(irq));\n"
               "  always @(posedge clk_a) q <= " + rd + ";\n"
               "endmodule\n";
    };
    auto runSpi = [&](const std::string& async, const std::string& rd) {
        test::TempDir dir;
        dir.write("ip/spi0/spi0.xml", component(ports));
        return run(project(dir, spiTop(rd),
                           {dir.write("ip/spi0/spi0.xci", spirit("spi0", "axi_quad_spi", "3.2",
                                                                 {{"Async_Clk", async}, {"C_FIFO_DEPTH", "16"}}))}));
    };
    // Async_Clk: the sides exchange through asynchronous FIFOs (recognized as such)
    auto r = runSpi("1", "rdata[7:0]");
    REQUIRE(r->cdc);
    CHECK(r->cdc->stats.paths >= 2);
    CHECK(r->cdc->stats.unsynchronized == 0);
    CHECK(std::ranges::count_if(r->cdc->structures, [](const auto& s) { return s.type == cdc::SyncType::AsyncFifo; }) == 2);
    // the SPI pins are on ext_spi_clk: read on clk_a they cross (positive)
    auto pins = runSpi("1", "{7'd0, mosi}");
    REQUIRE(pins->cdc);
    CHECK(pins->cdc->stats.unsynchronized >= 1);
    // without Async_Clk the IP expects one clock: different clocks cross unsynchronized
    auto direct = runSpi("0", "rdata[7:0]");
    REQUIRE(direct->cdc);
    CHECK(direct->cdc->stats.unsynchronized >= 1);
    CHECK(std::ranges::none_of(direct->cdc->structures, [](const auto& s) { return s.type == cdc::SyncType::AsyncFifo; }));
}

namespace {

// An IP of one's own in an IP repository: component.xml with a synthesis view,
// its HDL, and model parameters of three types; port `test_i` is disabled in
// the instance below and has the default value 1.
void writeUserIpRepo(const test::TempDir& dir, const std::string& version = "1.0", bool withFifo = false) {
    std::string files = "<spirit:file><spirit:name>hdl/blinker.v</spirit:name><spirit:fileType>verilogSource"
                        "</spirit:fileType></spirit:file>\n"
                        "<spirit:file><spirit:name>hdl/blinker_ooc.xdc</spirit:name><spirit:fileType>xdc"
                        "</spirit:fileType></spirit:file>\n";
    if (withFifo)
        files += "<spirit:file><spirit:name>hdl/bfifo/bfifo.xci</spirit:name><spirit:userFileType>xci"
                 "</spirit:userFileType></spirit:file>\n";
    dir.write("repo/blinker_1.0/component.xml",
              "<?xml version=\"1.0\"?>\n<spirit:component xmlns:spirit=\"http://www.spiritconsortium.org/XMLSchema/SPIRIT/"
              "1685-2009\">\n<spirit:vendor>example.com</spirit:vendor><spirit:library>user</spirit:library>"
              "<spirit:name>blinker</spirit:name><spirit:version>" + version + "</spirit:version>\n"
              "<spirit:busInterfaces><spirit:busInterface><spirit:name>other</spirit:name></spirit:busInterface>"
              "</spirit:busInterfaces>\n<spirit:model><spirit:views>\n"
              "<spirit:view><spirit:name>xilinx_anylanguagesynthesis</spirit:name><spirit:envIdentifier>:vivado.xilinx.com:"
              "synthesis</spirit:envIdentifier><spirit:modelName>blinker_top</spirit:modelName><spirit:fileSetRef>"
              "<spirit:localName>synth_fs</spirit:localName></spirit:fileSetRef></spirit:view>\n"
              "<spirit:view><spirit:name>xilinx_anylanguagebehavioralsimulation</spirit:name><spirit:envIdentifier>:vivado."
              "xilinx.com:simulation</spirit:envIdentifier><spirit:modelName>blinker_sim</spirit:modelName>"
              "<spirit:fileSetRef><spirit:localName>sim_fs</spirit:localName></spirit:fileSetRef></spirit:view>\n"
              "</spirit:views><spirit:ports>\n"
              "<spirit:port><spirit:name>clk</spirit:name><spirit:wire><spirit:direction>in</spirit:direction></spirit:wire></spirit:port>\n"
              "<spirit:port><spirit:name>d</spirit:name><spirit:wire><spirit:direction>in</spirit:direction></spirit:wire></spirit:port>\n"
              "<spirit:port><spirit:name>q</spirit:name><spirit:wire><spirit:direction>out</spirit:direction></spirit:wire></spirit:port>\n"
              "<spirit:port><spirit:name>test_i</spirit:name><spirit:wire><spirit:direction>in</spirit:direction>"
              "<spirit:driver><spirit:defaultValue spirit:format=\"long\">1</spirit:defaultValue></spirit:driver></spirit:wire></spirit:port>\n"
              "<spirit:port><spirit:name>test_o</spirit:name><spirit:wire><spirit:direction>out</spirit:direction></spirit:wire></spirit:port>\n"
              "</spirit:ports><spirit:modelParameters>\n"
              "<spirit:modelParameter spirit:dataType=\"string\"><spirit:name>MODE</spirit:name><spirit:value>A</spirit:value></spirit:modelParameter>\n"
              "<spirit:modelParameter spirit:dataType=\"integer\"><spirit:name>STAGES</spirit:name><spirit:value>1</spirit:value></spirit:modelParameter>\n"
              "<spirit:modelParameter spirit:dataType=\"reg\"><spirit:name>POLARITY</spirit:name><spirit:value>0</spirit:value></spirit:modelParameter>\n"
              "</spirit:modelParameters></spirit:model>\n<spirit:fileSets>\n"
              "<spirit:fileSet><spirit:name>sim_fs</spirit:name><spirit:file><spirit:name>hdl/blinker_sim.v</spirit:name>"
              "<spirit:fileType>verilogSource</spirit:fileType></spirit:file></spirit:fileSet>\n"
              "<spirit:fileSet><spirit:name>synth_fs</spirit:name>" + files + "</spirit:fileSet>\n"
              "</spirit:fileSets>\n</spirit:component>\n");
    // MODE "B" and POLARITY 1'b1 select the registered path; test_i must be 1 to pass d
    dir.write("repo/blinker_1.0/hdl/blinker.v",
              "module blinker_top #(parameter MODE = \"A\", parameter integer STAGES = 1, parameter POLARITY = 1'b0)\n"
              "  (input clk, input d, output q, input test_i, output test_o);\n"
              "  reg [STAGES-1:0] r = 0;\n"
              "  always @(posedge clk) r <= {r, d & test_i};\n"
              "  assign q = (MODE == \"B\" && POLARITY == 1'b1) ? r[STAGES-1] : 1'b0;\n"
              "  assign test_o = r[0];\n"
              "endmodule\n");
    dir.write("repo/blinker_1.0/hdl/blinker_ooc.xdc", "create_clock -period 10 [get_ports clk]\n");
    if (withFifo)
        dir.write("repo/blinker_1.0/hdl/bfifo/bfifo.xci", jsonFifo("bfifo"));
}

std::string blinkerInstance() {
    return spirit("u_blink_0", "blinker", "1.0", {{"MODE", "B"}, {"STAGES", "3"}, {"POLARITY", "&quot;1&quot;"}});
}

} // namespace

TEST_CASE("XCI: an IP of one's own from an IP repository", "[frontend][xci][iprepo]") {
    test::TempDir dir;
    writeUserIpRepo(dir);
    std::string xci = blinkerInstance();
    xci.replace(xci.find("spirit:vendor=\"xilinx.com\" spirit:library=\"ip\""), 46,
                "spirit:vendor=\"example.com\" spirit:library=\"user\"");
    dir.write("bd/u_blink_0/u_blink_0.xml", component({{"clk", "in", -1, true}, {"d", "in", -1, true},
                                                       {"q", "out", -1, true}, {"test_i", "in", -1, false},
                                                       {"test_o", "out", -1, false}}));
    frontend::XciContext ctx;
    ctx.ipRepositories = {dir.path() / "repo"};
    auto m = frontend::modelFromXci(dir.write("bd/u_blink_0/u_blink_0.xci", xci), ctx);
    REQUIRE(m);
    const auto& v = m.value().verilog;
    for (const char* p : {"module u_blink_0", "blinker_top #(.MODE(\"B\"), .STAGES(3), .POLARITY(1'b1)) inst",
                          ".clk(clk), .d(d), .q(q), .test_i(1), .test_o()"})
        CHECK(v.find(p) != std::string::npos);
    CHECK(v.find("blinker_sim") == std::string::npos); // the synthesis view, not the simulation one
    REQUIRE(m.value().extraSources.size() == 1);        // the HDL, not the .xdc
    CHECK(m.value().extraSources[0].filename() == "blinker.v");
    CHECK(m.value().ip == "example.com:user:blinker:1.0");

    // negative: no repository, another version, no instance component file
    auto none = frontend::modelFromXci(dir.path() / "bd/u_blink_0/u_blink_0.xci");
    REQUIRE_FALSE(none);
    CHECK(none.error().code == "FE-XCI-UNSUPPORTED");
    test::TempDir d2;
    writeUserIpRepo(d2, "2.0");
    frontend::XciContext ctx2;
    ctx2.ipRepositories = {d2.path() / "repo"};
    CHECK_FALSE(frontend::modelFromXci(dir.path() / "bd/u_blink_0/u_blink_0.xci", ctx2));
    test::TempDir d3;
    auto noPorts = frontend::modelFromXci(d3.write("u_blink_0.xci", xci), ctx);
    REQUIRE_FALSE(noPorts);
    CHECK(noPorts.error().message.find("found in the IP repository") != std::string::npos);

    // end to end: the IP's registers are analyzed; its output read on another clock crosses
    auto e2e = [&](const std::string& readClk, bool withFifo) {
        test::TempDir p;
        writeUserIpRepo(p, "1.0", withFifo);
        p.write("bd/u_blink_0/u_blink_0.xml", component({{"clk", "in", -1, true}, {"d", "in", -1, true},
                                                         {"q", "out", -1, true}, {"test_i", "in", -1, false},
                                                         {"test_o", "out", -1, false}}));
        engine::ProjectConfig cfg = project(p,
                                            "module top(input clk_a, input clk_b, input [7:0] d, input we, input re,\n"
                                            "           output reg [7:0] q, output reg [11:0] level);\n"
                                            "  wire b;\n"
                                            "  u_blink_0 u (.clk(clk_a), .d(we), .q(b));\n"
                                            "  always @(posedge " + readClk + ") q <= {7'd0, b};\n"
                                            "endmodule\n",
                                            {p.write("bd/u_blink_0/u_blink_0.xci", xci)});
        cfg.ipRepositories = {p.path() / "repo"};
        return run(cfg);
    };
    auto same = e2e("clk_a", false);
    REQUIRE(same->cdc);
    CHECK(same->cdc->stats.paths == 0);
    CHECK(count(*same, "FE-XCI-MODEL") == 1);
    CHECK(count(*same, "FE-UNCONNECTED-PORTS") == 0); // the disabled ports are tied in the wrapper
    auto other = e2e("clk_b", false);
    REQUIRE(other->cdc);
    CHECK(other->cdc->stats.unsynchronized == 1); // r[2] of the IP read on clk_b
    // an IP whose file set holds a further .xci: that one is modelled too
    auto nested = e2e("clk_a", true);
    CHECK(count(*nested, "FE-XCI-MODEL") == 2);
}

TEST_CASE("XCI: XADC Wizard, DRP interface in dclk_in", "[frontend][xci][xadc]") {
    // Digilent Arty A7 XADC demo: DRP, reset, VP/VN, VAUX4 and VAUX12, two alarms
    const std::vector<std::pair<std::string, std::string>> params = {
        {"C_HAS_AXI", "0"},        {"C_HAS_AXI4STREAM", "0"},   {"C_HAS_DRP", "1"},     {"C_HAS_DCLK", "1"},
        {"C_HAS_RESET", "1"},      {"C_HAS_VP", "1"},           {"C_HAS_VN", "1"},      {"C_VAUX4", "1"},
        {"C_VAUX12", "1"},         {"C_VAUX3", "0"},            {"C_HAS_VCCAUX_ALARM", "1"},
        {"C_HAS_VCCINT_ALARM", "1"}, {"C_HAS_OT_ALARM", "0"},   {"C_HAS_CHANNEL", "1"}, {"C_HAS_EOC", "1"},
        {"C_HAS_EOS", "1"},        {"C_HAS_BUSY", "1"},         {"C_HAS_CONVST", "0"},  {"C_DCLK_FREQUENCY", "100"}};
    auto m = frontend::modelFromXciText(spirit("xadc_wiz_0", "xadc_wiz", "3.3", params));
    REQUIRE(m);
    const auto& v = m.value().verilog;
    for (const char* p : {"input  wire [15:0] di_in", "input  wire [6:0] daddr_in", "input  wire den_in",
                          "output wire drdy_out", "output wire [15:0] do_out", "input  wire dclk_in",
                          "input  wire reset_in", "input  wire vp_in", "input  wire vauxp4", "input  wire vauxn12",
                          "output wire vccaux_alarm_out", "output wire vccint_alarm_out", "output wire [4:0] channel_out",
                          "output wire eoc_out", "output wire alarm_out", "output wire busy_out",
                          "always @(posedge dclk_in) oc_do_out"})
        CHECK(v.find(p) != std::string::npos);
    // negative: disabled channels and alarms; the analog inputs are not read
    for (const char* p : {"vauxp3", "ot_out", "convst_in", "user_temp_alarm_out", "^{di_in, daddr_in, den_in, vp_in"})
        CHECK(v.find(p) == std::string::npos);
    CHECK(m.value().summary.find("100 MHz") != std::string::npos);
    // not modelled: AXI4-Stream output; AXI4-Lite without a port list (XML)
    auto stream = params;
    stream[1].second = "1";
    CHECK_FALSE(frontend::modelFromXciText(spirit("x1", "xadc_wiz", "3.3", stream)));
    auto lite = params;
    lite[0].second = "1";
    auto l = frontend::modelFromXciText(spirit("x2", "xadc_wiz", "3.3", lite));
    REQUIRE_FALSE(l);
    CHECK(l.error().message.find("component file") != std::string::npos);

    // end to end: DRP data read on dclk_in does not cross, on another clock it does
    auto e2e = [&](const std::string& readClk) {
        test::TempDir dir;
        return run(project(dir,
                           "module top(input clk_a, input clk_b, input [7:0] d, input we, input re, output reg [7:0] q,\n"
                           "           output reg [11:0] level);\n"
                           "  wire [15:0] dout; wire rdy, eoc;\n"
                           "  xadc_wiz_0 u (.dclk_in(clk_a), .daddr_in(7'h16), .den_in(eoc), .di_in(16'd0), .dwe_in(1'b0),\n"
                           "    .reset_in(1'b0), .vp_in(1'b0), .vn_in(1'b0), .vauxp4(1'b0), .vauxn4(1'b0), .vauxp12(1'b0),\n"
                           "    .vauxn12(1'b0), .do_out(dout), .drdy_out(rdy), .eoc_out(eoc));\n"
                           "  always @(posedge " + readClk + ") if (rdy) level <= dout[15:4];\n"
                           "endmodule\n",
                           {dir.write("xadc_wiz_0.xci", spirit("xadc_wiz_0", "xadc_wiz", "3.3", params))}));
    };
    auto same = e2e("clk_a");
    REQUIRE(same->cdc);
    CHECK(same->cdc->stats.paths == 0);
    auto other = e2e("clk_b");
    REQUIRE(other->cdc);
    CHECK(other->cdc->stats.unsynchronized >= 1);
}

namespace {

// Transceivers Wizard (gtwizard_ultrascale) after SURF's Pgp3GthUsIp10G:
// 10.3125 Gb/s, user clocks 312.5 / 156.25 MHz, ports enabled by the configuration.
std::vector<std::pair<std::string, std::string>> gtModel(int gtType, int channels, bool frequencies = true) {
    std::vector<std::pair<std::string, std::string>> m = {
        {"C_GT_TYPE", std::to_string(gtType)},       {"C_TOTAL_NUM_CHANNELS", std::to_string(channels)},
        {"C_TX_LINE_RATE", "10.3125"},               {"C_TX_USER_DATA_WIDTH", "64"},
        {"C_RX_USER_DATA_WIDTH", "64"},              {"C_TX_USER_CLOCKING_RATIO_FSRC_FUSRCLK", "1"},
        {"C_RX_USER_CLOCKING_RATIO_FSRC_FUSRCLK", "1"}};
    if (frequencies)
        for (const char* d : {"TX", "RX"}) {
            m.emplace_back(std::string("C_") + d + "_USRCLK_FREQUENCY", "312.5000000");
            m.emplace_back(std::string("C_") + d + "_USRCLK2_FREQUENCY", "156.25");
        }
    return m;
}

std::vector<std::pair<std::string, std::string>> gtUser(const std::vector<std::string>& extra = {}) {
    std::vector<std::pair<std::string, std::string>> u = {
        {"ENABLE_OPTIONAL_PORTS", "drpaddr_in drpclk_in drpdi_in drpen_in drpwe_in txdiffctrl_in drpdo_out drprdy_out"}};
    for (const char* p : {"GTHRXN_IN", "GTHRXP_IN", "GTHTXN_OUT", "GTHTXP_OUT", "GTWIZ_USERCLK_TX_SRCCLK_OUT",
                          "GTWIZ_USERCLK_TX_USRCLK_OUT", "GTWIZ_USERCLK_TX_USRCLK2_OUT", "GTWIZ_USERCLK_RX_SRCCLK_OUT",
                          "GTWIZ_USERCLK_RX_USRCLK_OUT", "GTWIZ_USERCLK_RX_USRCLK2_OUT", "GTWIZ_USERDATA_TX_IN",
                          "GTWIZ_USERDATA_RX_OUT", "TXHEADER_IN", "RXPMARESETDONE_OUT", "GTWIZ_RESET_CLK_FREERUN_IN",
                          "GTWIZ_RESET_ALL_IN", "GTWIZ_RESET_TX_DONE_OUT", "GTWIZ_RESET_RX_DONE_OUT"})
        u.emplace_back(std::string("INTERNAL_PORT_ENABLED_") + p, "1");
    u.emplace_back("INTERNAL_PORT_ENABLED_RXSLIDE_IN", "-1"); // disabled
    for (const auto& e : extra)
        u.emplace_back("INTERNAL_PORT_ENABLED_" + e, "1");
    return u;
}

} // namespace

TEST_CASE("XCI: Transceivers Wizard ports, widths and user clocks from the configuration", "[frontend][xci][gt]") {
    auto m = frontend::modelFromXciText(spirit("gt_10g", "gtwizard_ultrascale", "1.7", gtModel(2, 2), gtUser()));
    REQUIRE(m);
    const auto& v = m.value().verilog;
    INFO(v);
    // two GTHE4 channels: widths per channel times 2
    for (const char* p : {"input  wire [127:0] gtwiz_userdata_tx_in", "output wire [127:0] gtwiz_userdata_rx_out",
                          "input  wire [19:0] drpaddr_in", "input  wire [9:0] txdiffctrl_in",
                          "input  wire [11:0] txheader_in", "output wire [1:0] gthtxp_out", "input  wire [1:0] drpclk_in",
                          "output wire [0:0] gtwiz_userclk_rx_usrclk2_out", "output wire [1:0] rxpmaresetdone_out"})
        CHECK(v.find(p) != std::string::npos);
    // the source clock (312.5 MHz) and the dividers through the source clock port
    CHECK(v.find("OPENCDC_CLOCK_PIN = \"gtwiz_userclk_tx_srcclk_out\", OPENCDC_CLOCK_PERIOD = 3.2") != std::string::npos);
    CHECK(v.find("OPENCDC_CLOCK_PIN = \"gtwiz_userclk_rx_usrclk2_out\", OPENCDC_CLOCK_RATIO = 2.0") != std::string::npos);
    CHECK(v.find("always @(posedge gtwiz_userclk_rx_srcclk_out[0])") != std::string::npos);
    // TX data in the TX user clock, DRP in drpclk, PMA reset done asynchronous
    CHECK(v.find("^{gtwiz_userdata_tx_in, txheader_in}") != std::string::npos);
    CHECK(v.find("always @(posedge drpclk_in[0]) oc_drp") != std::string::npos);
    CHECK(v.find("opencdc_async_source u_oc_rxpmaresetdone_out") != std::string::npos);
    // negative: a disabled port (-1) and ports that are not enabled
    for (const char* p : {"rxslide_in", "txctrl0_in", "cpllfbclklost_out"})
        CHECK(v.find(p) == std::string::npos);
    CHECK(m.value().summary.find("2 channel(s), GTHE4, line rate 10.3125 Gb/s") != std::string::npos);
    CHECK(m.value().summary.find("rx usrclk2 156.25 MHz") != std::string::npos);

    // GTHE3: 9-bit DRP address and 4-bit TX swing control per channel
    auto e3 = frontend::modelFromXciText(spirit("gt_e3", "gtwizard_ultrascale", "1.7", gtModel(0, 2), gtUser()));
    REQUIRE(e3);
    CHECK(e3.value().verilog.find("input  wire [17:0] drpaddr_in") != std::string::npos);
    CHECK(e3.value().verilog.find("input  wire [7:0] txdiffctrl_in") != std::string::npos);
    // GTYE4: serial pins named gty*
    auto y4 = frontend::modelFromXciText(spirit("gt_y4", "gtwizard_ultrascale", "1.7", gtModel(3, 1),
                                                {{"INTERNAL_PORT_ENABLED_GTYRXN_IN", "1"},
                                                 {"INTERNAL_PORT_ENABLED_GTYTXP_OUT", "1"}}));
    REQUIRE(y4);
    CHECK(y4.value().verilog.find("input  wire [0:0] gtyrxn_in") != std::string::npos);
}

TEST_CASE("XCI: Transceivers Wizard with user clocking outside the IP and buffer bypass", "[frontend][xci][gt]") {
    // SURF Pgp2fc: TX/RX user clocks are inputs, the buffer bypass helper, CPLL controls
    auto user = gtUser({"RXUSRCLK_IN", "RXUSRCLK2_IN", "TXUSRCLK_IN", "TXUSRCLK2_IN", "GTWIZ_BUFFBYPASS_RX_RESET_IN",
                        "GTWIZ_BUFFBYPASS_RX_START_USER_IN", "GTWIZ_BUFFBYPASS_RX_DONE_OUT", "RXSYNCDONE_OUT",
                        "CPLLREFCLKSEL_IN", "CPLLFBCLKLOST_OUT", "RXPMARESET_IN", "TXINHIBIT_IN"});
    std::erase_if(user, [](const auto& kv) { return kv.first.find("GTWIZ_USERCLK_") != std::string::npos; });
    auto m = frontend::modelFromXciText(spirit("gt_fc", "gtwizard_ultrascale", "1.7", gtModel(2, 1), user));
    if (!m)
        UNSCOPED_INFO(m.error().message);
    REQUIRE(m);
    const auto& v = m.value().verilog;
    INFO(v);
    CHECK(v.find("input  wire [2:0] cpllrefclksel_in") != std::string::npos);
    CHECK(v.find("always @(posedge rxusrclk2_in[0]) oc_rx <= ^{gtwiz_buffbypass_rx_start_user_in}") != std::string::npos);
    CHECK(v.find("always @(posedge rxusrclk2_in[0]) oc_gtwiz_buffbypass_rx_done_out_q") != std::string::npos);
    CHECK(v.find("opencdc_async_source u_oc_rxsyncdone_out") != std::string::npos);
    CHECK(v.find("opencdc_async_source u_oc_cpllfbclklost_out") != std::string::npos);
    // negative: asynchronous resets and static controls are read by no domain register
    std::string mixed; // the inputs of every domain register
    for (std::size_t at = v.find("<= ^{"); at != std::string::npos; at = v.find("<= ^{", at + 1))
        mixed += v.substr(at, v.find(';', at) - at);
    CHECK(mixed.find("gtwiz_buffbypass_rx_start_user_in") != std::string::npos);
    for (const char* p : {"gtwiz_buffbypass_rx_reset_in", "rxpmareset_in", "txinhibit_in", "cpllrefclksel_in"})
        CHECK(mixed.find(p) == std::string::npos);
    CHECK(m.value().summary.find("user clocking outside the IP") != std::string::npos);
}

TEST_CASE("XCI negative: Transceivers Wizard configurations that are not modelled", "[frontend][xci][gt]") {
    // a port whose width and domain are unknown is named in the message
    auto unknown = frontend::modelFromXciText(
        spirit("gt_u", "gtwizard_ultrascale", "1.7", gtModel(2, 1), gtUser({"RXOSINTDONE_OUT"})));
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == "FE-XCI-UNSUPPORTED");
    CHECK(unknown.error().message.find("rxosintdone_out") != std::string::npos);
    // user clock frequencies missing: no invented period
    auto noFreq = frontend::modelFromXciText(spirit("gt_f", "gtwizard_ultrascale", "1.7", gtModel(2, 1, false), gtUser()));
    REQUIRE_FALSE(noFreq);
    CHECK(noFreq.error().message.find("C_TX_USRCLK_FREQUENCY") != std::string::npos);
    // no port enabled at all
    auto none = frontend::modelFromXciText(spirit("gt_n", "gtwizard_ultrascale", "1.7", gtModel(2, 1),
                                                  {{"INTERNAL_PORT_ENABLED_GTHRXN_IN", "-1"}}));
    REQUIRE_FALSE(none);
    CHECK(none.error().message.find("names no ports") != std::string::npos);
}

TEST_CASE("XCI: Transceivers Wizard user clocks in a design", "[frontend][xci][gt]") {
    // RX data registered in the RX usrclk2, then read in clk_a: a crossing only in the second case
    auto e2e = [](const std::string& readClk, const std::string& sdc) {
        test::TempDir dir;
        engine::ProjectConfig cfg;
        cfg.name = "gt";
        cfg.top = "top";
        cfg.sources = {
            dir.write("top.v",
                      "module top(input clk_a, input free, input rxn, input rxp, output reg [7:0] q);\n"
                      "  wire [63:0] rxd; wire rxclk, txclk; wire [0:0] txn, txp; wire [11:0] hdr;\n"
                      "  wire [9:0] drpa; wire [15:0] drpdo; wire drprdy, tdone, rdone, pma;\n"
                      "  gt_10g u (.gthrxn_in(rxn), .gthrxp_in(rxp), .gthtxn_out(txn), .gthtxp_out(txp),\n"
                      "    .gtwiz_userclk_tx_srcclk_out(), .gtwiz_userclk_tx_usrclk_out(), .gtwiz_userclk_tx_usrclk2_out(txclk),\n"
                      "    .gtwiz_userclk_rx_srcclk_out(), .gtwiz_userclk_rx_usrclk_out(), .gtwiz_userclk_rx_usrclk2_out(rxclk),\n"
                      "    .gtwiz_userdata_tx_in(64'd0), .gtwiz_userdata_rx_out(rxd), .txheader_in(6'd0),\n"
                      "    .rxpmaresetdone_out(pma), .gtwiz_reset_clk_freerun_in(free), .gtwiz_reset_all_in(1'b0),\n"
                      "    .gtwiz_reset_tx_done_out(tdone), .gtwiz_reset_rx_done_out(rdone), .drpaddr_in(10'd0),\n"
                      "    .drpclk_in(free), .drpdi_in(16'd0), .drpen_in(1'b0), .drpwe_in(1'b0), .txdiffctrl_in(5'd0),\n"
                      "    .drpdo_out(drpdo), .drprdy_out(drprdy));\n"
                      "  reg [7:0] r; always @(posedge rxclk) r <= rxd[7:0];\n"
                      "  always @(posedge " + readClk + ") q <= r;\n"
                      "endmodule\n"),
            dir.write("gt_10g.xci", spirit("gt_10g", "gtwizard_ultrascale", "1.7", gtModel(2, 1), gtUser()))};
        cfg.sourceLibraries = {"", ""};
        cfg.constraints = {dir.write("c.sdc", sdc)};
        return run(cfg);
    };
    const std::string base = "create_clock -name a -period 10 [get_ports clk_a]\n"
                             "create_clock -name free -period 8 [get_ports free]\n";
    auto same = e2e("rxclk", base);
    REQUIRE(same->cdc);
    CHECK(same->cdc->stats.paths == 0);
    // the RX usrclk2: 156.25 MHz, a clock of its own derived from the RX source clock
    REQUIRE(same->clocks);
    bool usr2 = false;
    for (const auto& c : same->clocks->clocks)
        if (c.name.find("gtwiz_userclk_rx_usrclk2_out") != std::string::npos && c.periodNs) {
            CHECK((*c.periodNs > 6.39 && *c.periodNs < 6.41));
            usr2 = true;
        }
    CHECK(usr2);
    // Vivado style: the user clocks asynchronous to clk_a by get_clocks -of_objects on the source pin
    auto other = e2e("clk_a", base + "set_clock_groups -asynchronous -group a -group free "
                                     "-group [get_clocks -of_objects [get_pins u/gtwiz_userclk_rx_srcclk_out*]]\n");
    REQUIRE(other->cdc);
    CHECK(other->cdc->stats.unsynchronized == 1);
    CHECK(other->cdc->stats.unknownRelation == 0);
    CHECK(count(*other, "CON-UNMATCHED") == 0);
}
