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

TEST_CASE("XCI: VHDL component declarations give IP ports", "[frontend][xci][component]") {
    const std::string vhdl = "library ieee;\nuse ieee.std_logic_1164.all;\n"
                             "package p is\n"
                             "  component ip_a\n"
                             "    port (clk : in std_logic; d : in std_logic_vector(16-1 downto 0);\n"
                             "          q : out std_logic_vector(0 to 3); io : inout std_logic; b : buffer bit;\n"
                             "          u : out ieee.std_logic_1164.std_ulogic);\n"
                             "  end component;\n"
                             "  component ip_generic\n" // a width from a generic: not guessed
                             "    generic (W : natural := 8);\n"
                             "    port (clk : in std_logic; d : in std_logic_vector(W-1 downto 0));\n"
                             "  end component;\n"
                             "  component ip_twice port (a : in std_logic); end component;\n"
                             "end package;\n"
                             "library ieee;\nuse ieee.std_logic_1164.all;\n"
                             "entity e is end;\n"
                             "architecture r of e is\n"
                             "  component IP_A\n" // the same component again: identical, kept
                             "    port (clk : in std_logic; d : in std_logic_vector(15 downto 0);\n"
                             "          q : out std_logic_vector(0 to 3); io : inout std_logic; b : buffer bit;\n"
                             "          u : out ieee.std_logic_1164.std_ulogic);\n"
                             "  end component;\n"
                             "  component ip_twice port (a : in std_logic_vector(1 downto 0)); end component;\n"
                             "begin\nend;\n";
    const auto c = frontend::vhdlComponentPorts(vhdl, "p.vhd");
    REQUIRE(c.contains("ip_a"));
    const auto& a = c.at("ip_a");
    REQUIRE(a.ports.size() == 6);
    CHECK(a.where == "p.vhd:4");
    CHECK((a.ports[0].name == "clk" && !a.ports[0].output && !a.ports[0].range));
    CHECK((a.ports[1].range == std::pair<long long, long long>{15, 0}));
    CHECK((a.ports[2].output && a.ports[2].range == std::pair<long long, long long>{0, 3}));
    CHECK(a.ports[3].inout);
    CHECK((a.ports[4].output && !a.ports[4].range));
    CHECK((a.ports[5].output && !a.ports[5].range));
    // negative: a generic width, and two different declarations of one name
    CHECK_FALSE(c.contains("ip_generic"));
    CHECK_FALSE(c.contains("ip_twice"));
}

namespace {

// 7-series Transceivers Wizard after SURF's Pgp3Gtx7Ip6G (gtwizard 3.6, GTX, 6.25 Gb/s, 64B/66B)
std::string gt7Xci(const std::string& inst) {
    return spirit(inst, "gtwizard", "3.6",
                  {{"gt_type", "GTX"},                      {"gt0_val", "true"},
                   {"gt0_val_tx_line_rate", "6.25"},        {"gt0_val_rx_line_rate", "6.25"},
                   {"gt0_val_tx_int_datawidth", "32"},      {"gt0_val_rx_int_datawidth", "32"},
                   {"gt0_val_tx_reference_clock", "156.250"}, {"gt0_val_rx_reference_clock", "156.250"},
                   {"gt0_val_txoutclk_source", "AUTO"},     {"gt0_val_rxoutclk_source", "AUTO"}});
}

std::string gt7Component(const std::string& inst, const std::string& extra = "", bool txClock = true) {
    return "  component " + inst + "\n    port (\n"
           "      SYSCLK_IN : in std_logic; SOFT_RESET_TX_IN : in std_logic;\n"
           "      GT0_TX_FSM_RESET_DONE_OUT : out std_logic; GT0_QPLLRESET_OUT : out std_logic;\n"
           "      gt0_drpaddr_in : in std_logic_vector(8 downto 0); gt0_drpclk_in : in std_logic;\n"
           "      gt0_drpen_in : in std_logic; gt0_drpdo_out : out std_logic_vector(15 downto 0);\n"
           "      gt0_drprdy_out : out std_logic;\n"
           "      gt0_rxusrclk_in : in std_logic; gt0_rxusrclk2_in : in std_logic;\n"
           "      gt0_rxdata_out : out std_logic_vector(63 downto 0);\n"
           "      gt0_gtxrxp_in : in std_logic; gt0_gtxrxn_in : in std_logic;\n"
           "      gt0_rxoutclk_out : out std_logic; gt0_rxoutclkfabric_out : out std_logic;\n"
           "      gt0_eyescandataerror_out : out std_logic; gt0_loopback_in : in std_logic_vector(2 downto 0);\n" +
           (txClock ? "      gt0_txusrclk_in : in std_logic; gt0_txusrclk2_in : in std_logic;\n" : "") + extra +
           "      gt0_txdata_in : in std_logic_vector(63 downto 0);\n"
           "      gt0_gtxtxp_out : out std_logic; gt0_gtxtxn_out : out std_logic);\n"
           "  end component;\n";
}

std::string gt7Top(const std::string& readClk) {
    return "library ieee;\nuse ieee.std_logic_1164.all;\n"
           "entity top is\n  port (sysclk, txclk, clk_a, rxp, rxn : in std_logic; q : out std_logic_vector(7 downto 0));\n"
           "end;\narchitecture rtl of top is\n" +
           gt7Component("gt7") +
           "  signal rxclk : std_logic;\n  signal rxd : std_logic_vector(63 downto 0);\n"
           "  signal r : std_logic_vector(7 downto 0);\nbegin\n"
           "  u : gt7 port map (SYSCLK_IN => sysclk, SOFT_RESET_TX_IN => '0', GT0_TX_FSM_RESET_DONE_OUT => open,\n"
           "    GT0_QPLLRESET_OUT => open, gt0_drpaddr_in => (others => '0'), gt0_drpclk_in => sysclk,\n"
           "    gt0_drpen_in => '0', gt0_drpdo_out => open, gt0_drprdy_out => open,\n"
           "    gt0_rxusrclk_in => rxclk, gt0_rxusrclk2_in => rxclk, gt0_rxdata_out => rxd, gt0_gtxrxp_in => rxp,\n"
           "    gt0_gtxrxn_in => rxn, gt0_rxoutclk_out => rxclk, gt0_rxoutclkfabric_out => open,\n"
           "    gt0_eyescandataerror_out => open, gt0_loopback_in => \"000\", gt0_txusrclk_in => txclk,\n"
           "    gt0_txusrclk2_in => txclk, gt0_txdata_in => (others => '0'), gt0_gtxtxp_out => open,\n"
           "    gt0_gtxtxn_out => open);\n"
           "  process (rxclk) begin if rising_edge(rxclk) then r <= rxd(7 downto 0); end if; end process;\n"
           "  process (" + readClk + ") begin if rising_edge(" + readClk + ") then q <= r; end if; end process;\n"
           "end;\n";
}

} // namespace

TEST_CASE("XCI: 7-series Transceivers Wizard from a VHDL component declaration", "[frontend][xci][gt]") {
    test::TempDir dir;
    const auto xci = dir.write("gt7.xci", gt7Xci("gt7"));
    frontend::XciContext ctx;
    ctx.vhdlComponents = frontend::vhdlComponentPorts(gt7Top("rxclk"), "top.vhd");
    auto m = frontend::modelFromXci(xci, ctx);
    REQUIRE(m);
    const auto& v = m.value().verilog;
    INFO(v);
    for (const char* p : {"input  wire [8:0] gt0_drpaddr_in", "output wire [63:0] gt0_rxdata_out",
                          "input  wire [0:0] SYSCLK_IN", "output wire [0:0] GT0_TX_FSM_RESET_DONE_OUT",
                          // RXOUTCLK: 6.25 Gb/s / 32 bits = 195.3125 MHz; the fabric clock: the reference clock
                          "OPENCDC_CLOCK_PIN = \"gt0_rxoutclk_out\", OPENCDC_CLOCK_PERIOD = 5.12",
                          "OPENCDC_CLOCK_PIN = \"gt0_rxoutclkfabric_out\", OPENCDC_CLOCK_PERIOD = 6.4",
                          // the reset state machines on sysclk_in, user data on the channel's usrclk2, DRP on drpclk
                          "always @(posedge SYSCLK_IN[0]) oc_GT0_TX_FSM_RESET_DONE_OUT_q",
                          "always @(posedge SYSCLK_IN[0]) oc_GT0_QPLLRESET_OUT_q",
                          "always @(posedge gt0_rxusrclk2_in[0]) oc_gt0_rxdata_out_q",
                          "always @(posedge gt0_txusrclk2_in[0]) oc_gt0_txusrclk2_in <= ^{gt0_txdata_in}",
                          "always @(posedge gt0_drpclk_in[0]) oc_gt0_drpclk_in <= ^{gt0_drpaddr_in, gt0_drpen_in}",
                          "opencdc_async_source u_oc_gt0_eyescandataerror_out"})
        CHECK(v.find(p) != std::string::npos);
    // negative: resets, analog controls and serial pins are read by no register
    std::string mixed; // the inputs of every domain register
    for (std::size_t at = v.find("<= ^{"); at != std::string::npos; at = v.find("<= ^{", at + 1))
        mixed += v.substr(at, v.find(';', at) - at);
    CHECK(mixed.find("gt0_txdata_in") != std::string::npos);
    for (const char* p : {"SOFT_RESET_TX_IN", "gt0_loopback_in", "gt0_gtxrxp_in", "gt0_rxusrclk_in"})
        CHECK(mixed.find(p) == std::string::npos);
    CHECK(m.value().summary.find("1 channel(s), 7-series GTX, line rate 6.25 Gb/s, gt0_rxoutclk_out 195.3125 MHz") !=
          std::string::npos);
    CHECK(std::ranges::any_of(m.value().notes, [](const std::string& n) {
        return n.starts_with("ports from the VHDL component declaration at top.vhd:");
    }));
}

TEST_CASE("XCI negative: 7-series Transceivers Wizard without usable ports", "[frontend][xci][gt]") {
    test::TempDir dir;
    const auto xci = dir.write("gt7.xci", gt7Xci("gt7"));
    // no port list at all: the message names the three ways to give one
    auto none = frontend::modelFromXci(xci, frontend::XciContext{});
    REQUIRE_FALSE(none);
    CHECK(none.error().code == "FE-XCI-UNSUPPORTED");
    CHECK(none.error().message.find("VHDL component declaration") != std::string::npos);
    auto wrap = [](const std::string& comp) {
        return "library ieee;\nuse ieee.std_logic_1164.all;\npackage p is\n" + comp + "end package;\n";
    };
    // a port that is not modelled is named
    frontend::XciContext odd;
    odd.vhdlComponents = frontend::vhdlComponentPorts(
        wrap(gt7Component("gt7", "      gt0_rxosintdone_out : out std_logic;\n")), "p.vhd");
    auto u = frontend::modelFromXci(xci, odd);
    REQUIRE_FALSE(u);
    CHECK(u.error().message.find("gt0_rxosintdone_out") != std::string::npos);
    // TX data without the TX user clock port
    frontend::XciContext noClk;
    noClk.vhdlComponents = frontend::vhdlComponentPorts(wrap(gt7Component("gt7", "", false)), "p.vhd");
    auto c = frontend::modelFromXci(xci, noClk);
    REQUIRE_FALSE(c);
    CHECK(c.error().message.find("without their clock port: gt0_txusrclk2_in") != std::string::npos);
}

TEST_CASE("XCI: 7-series Transceivers Wizard in a VHDL design", "[frontend][xci][gt]") {
    auto e2e = [](const std::string& readClk, const std::string& sdc) {
        test::TempDir dir;
        engine::ProjectConfig cfg;
        cfg.name = "gt7";
        cfg.top = "top";
        cfg.sources = {dir.write("top.vhd", gt7Top(readClk)), dir.write("gt7.xci", gt7Xci("gt7"))};
        cfg.sourceLibraries = {"", ""};
        cfg.constraints = {dir.write("c.sdc", sdc)};
        return run(cfg);
    };
    const std::string base = "create_clock -name a -period 10 [get_ports clk_a]\n"
                             "create_clock -name sys -period 8 [get_ports sysclk]\n"
                             "create_clock -name tx -period 5.12 [get_ports txclk]\n";
    auto same = e2e("rxclk", base);
    REQUIRE(same->cdc);
    CHECK(same->cdc->stats.paths == 0);
    CHECK(count(*same, "FE-XCI-MODEL") == 1);
    REQUIRE(same->clocks);
    bool rx = false;
    for (const auto& c : same->clocks->clocks)
        if (c.name.find("gt0_rxoutclk_out") != std::string::npos && c.periodNs) {
            CHECK((*c.periodNs > 5.11 && *c.periodNs < 5.13));
            rx = true;
        }
    CHECK(rx);
    // the RX clock asynchronous to the others by get_clocks -of_objects on the IP's pin, as in Vivado
    auto other = e2e("clk_a", base + "set_clock_groups -asynchronous -group a -group sys -group tx "
                                     "-group [get_clocks -of_objects [get_pins u/gt0_rxoutclk_out]]\n");
    REQUIRE(other->cdc);
    CHECK(other->cdc->stats.unsynchronized == 1);
    CHECK(other->cdc->stats.unknownRelation == 0);
    CHECK(count(*other, "CON-UNMATCHED") == 0);
}

namespace {

std::string pkg(const std::string& components) {
    return "library ieee;\nuse ieee.std_logic_1164.all;\npackage p is\n" + components + "end package;\n";
}

// SURF TenGigEthGtx7Core / TenGigEthGthUltraScale156p25MHzCore (ten_gig_eth_pcs_pma 6.0), the ports used
std::string tenGigComponent(const std::string& inst, bool ultrascale) {
    return "  component " + inst + "\n    port (\n"
           "      rxrecclk_out : out std_logic; coreclk : in std_logic; dclk : in std_logic;\n"
           "      txusrclk : in std_logic; txusrclk2 : in std_logic; txoutclk : out std_logic;\n"
           "      areset : in std_logic; areset_coreclk : in std_logic; gttxreset : in std_logic;\n"
           "      txp : out std_logic; txn : out std_logic; rxp : in std_logic; rxn : in std_logic;\n"
           "      xgmii_txd : in std_logic_vector(63 downto 0); xgmii_txc : in std_logic_vector(7 downto 0);\n"
           "      xgmii_rxd : out std_logic_vector(63 downto 0); xgmii_rxc : out std_logic_vector(7 downto 0);\n"
           "      configuration_vector : in std_logic_vector(535 downto 0);\n"
           "      core_status : out std_logic_vector(7 downto 0); signal_detect : in std_logic;\n"
           "      drp_req : out std_logic; drp_gnt : in std_logic; drp_den_o : out std_logic;\n" +
           std::string(ultrascale ? "      qpll0reset : out std_logic; gt_txbufstatus : out std_logic_vector(1 downto 0);\n"
                                    "      gt_rxprbserr : out std_logic; gt_eyescandataerror : out std_logic;\n"
                                    "      gt_txdiffctrl : in std_logic_vector(3 downto 0);\n"
                                  : "      gt0_txbufstatus : out std_logic_vector(1 downto 0); gt0_rxprbserr : out std_logic;\n"
                                    "      gt0_dmonitorout : out std_logic_vector(7 downto 0);\n") +
           "      tx_disable : out std_logic);\n  end component;\n";
}

std::string tenGigXci(const std::string& inst, const std::string& family, const std::string& width,
                      const std::string& noEbuff = "false") {
    return spirit(inst, "ten_gig_eth_pcs_pma", "6.0",
                  {{"c_family", family}, {"c_gtif_width", width}, {"c_speed10_25", "10"}, {"c_no_ebuff", noEbuff},
                   {"c_is_kr", "false"}});
}

} // namespace

TEST_CASE("XCI: 10G Ethernet PCS/PMA (ten_gig_eth_pcs_pma) clock domains", "[frontend][xci][ethernet]") {
    test::TempDir dir;
    // 7 series: XGMII, configuration and status in coreclk
    frontend::XciContext c7;
    c7.vhdlComponents = frontend::vhdlComponentPorts(pkg(tenGigComponent("tg7", false)), "p.vhd");
    auto m7 = frontend::modelFromXci(dir.write("tg7.xci", tenGigXci("tg7", "kintex7", "32")), c7);
    REQUIRE(m7);
    const auto& v7 = m7.value().verilog;
    INFO(v7);
    for (const char* p : {"OPENCDC_CLOCK_PIN = \"txoutclk\", OPENCDC_CLOCK_PERIOD = 3.103030303",
                          "OPENCDC_CLOCK_PIN = \"rxrecclk_out\", OPENCDC_CLOCK_PERIOD = 3.103030303",
                          "always @(posedge coreclk[0]) oc_dom_coreclk <= ^{xgmii_txd, xgmii_txc, configuration_vector}",
                          "always @(posedge coreclk[0]) oc_xgmii_rxd_q", "always @(posedge coreclk[0]) oc_core_status_q",
                          "always @(posedge dclk[0]) oc_drp_req_q", "always @(posedge txusrclk2[0]) oc_gt0_txbufstatus_q",
                          "always @(posedge oc_rxrecclk_out_c) oc_gt0_rxprbserr_q",
                          "opencdc_async_source u_oc_gt0_dmonitorout"})
        CHECK(v7.find(p) != std::string::npos);
    // negative: resets and signal_detect are synchronized in the IP, read by no register
    for (const char* p : {"areset", "signal_detect", "gttxreset"})
        CHECK(v7.find(std::string("^{") + p) == std::string::npos);
    CHECK(m7.value().summary.find("XGMII in coreclk") != std::string::npos);

    // UltraScale 10GBASE-R: the TX clock source is txusrclk2; coreclk only runs the reset logic
    frontend::XciContext cu;
    cu.vhdlComponents = frontend::vhdlComponentPorts(pkg(tenGigComponent("tgu", true)), "p.vhd");
    auto mu = frontend::modelFromXci(dir.write("tgu.xci", tenGigXci("tgu", "kintexu", "64")), cu);
    REQUIRE(mu);
    const auto& vu = mu.value().verilog;
    INFO(vu);
    CHECK(vu.find("always @(posedge txusrclk2[0]) oc_xgmii_rxd_q") != std::string::npos);
    CHECK(vu.find("always @(posedge coreclk[0])") == std::string::npos); // negative
    CHECK(vu.find("opencdc_async_source u_oc_qpll0reset") != std::string::npos);
    CHECK(vu.find("OPENCDC_CLOCK_PIN = \"txoutclk\", OPENCDC_CLOCK_PERIOD = 3.103030303") != std::string::npos);

    // without the elastic buffer: XGMII RX in the recovered clock
    auto mn = frontend::modelFromXci(dir.write("tgn.xci", tenGigXci("tg7", "kintex7", "32", "true")), c7);
    REQUIRE(mn);
    CHECK(mn.value().verilog.find("always @(posedge oc_rxrecclk_out_c) oc_xgmii_rxd_q") != std::string::npos);

    // negative: no ports, an unknown port
    auto none = frontend::modelFromXci(dir.write("tgx.xci", tenGigXci("tgx", "kintex7", "32")), frontend::XciContext{});
    REQUIRE_FALSE(none);
    CHECK(none.error().message.find("VHDL component declaration") != std::string::npos);
    frontend::XciContext odd;
    odd.vhdlComponents = frontend::vhdlComponentPorts(
        pkg("  component tg7 port (coreclk : in std_logic; mdc : in std_logic; mdio_in : in std_logic); end component;\n"),
        "p.vhd");
    auto u = frontend::modelFromXci(dir.write("tgo.xci", tenGigXci("tg7", "kintex7", "32")), odd);
    REQUIRE_FALSE(u);
    CHECK(u.error().message.find("mdc, mdio_in") != std::string::npos);
}

namespace {

std::string xxvXci(const std::string& inst, const std::string& core = "Ethernet PCS/PMA 64-bit",
                   const std::string& cores = "1") {
    return spirit(inst, "xxv_ethernet", "2.5",
                  {{"C_CORE", core}, {"C_DATA_PATH_INTERFACE", "MII"}, {"C_NUM_OF_CORES", cores}, {"C_LINE_RATE", "10"},
                   {"C_CLOCKING", "Asynchronous"}});
}

// SURF TenGigEthGthUltraScale156p25MHzCore (xxv_ethernet 2.5), the ports used
const char* kXxvComponent =
    "  component xxv\n    port (\n"
    "      gt_txp_out : out std_logic_vector(0 downto 0); gt_rxp_in : in std_logic_vector(0 downto 0);\n"
    "      rx_core_clk_0 : in std_logic; rx_serdes_reset_0 : in std_logic; gt_rxpolarity_0 : in std_logic;\n"
    "      gt_txbufstatus_0 : out std_logic_vector(1 downto 0); gt_dmonitorout_0 : out std_logic_vector(16 downto 0);\n"
    "      rxrecclkout_0 : out std_logic; gt_drpclk_0 : in std_logic; gt_drpdo_0 : out std_logic_vector(15 downto 0);\n"
    "      gt_drpen_0 : in std_logic; sys_reset : in std_logic; dclk : in std_logic;\n"
    "      tx_mii_clk_0 : out std_logic; rx_clk_out_0 : out std_logic; gtpowergood_out_0 : out std_logic;\n"
    "      gt_reset_tx_done_out_0 : out std_logic; rx_reset_0 : in std_logic;\n"
    "      rx_mii_d_0 : out std_logic_vector(63 downto 0); rx_mii_c_0 : out std_logic_vector(7 downto 0);\n"
    "      ctl_rx_test_pattern_0 : in std_logic; stat_rx_block_lock_0 : out std_logic;\n"
    "      tx_reset_0 : in std_logic; tx_mii_d_0 : in std_logic_vector(63 downto 0);\n"
    "      tx_mii_c_0 : in std_logic_vector(7 downto 0); stat_tx_local_fault_0 : out std_logic;\n"
    "      ctl_tx_test_pattern_0 : in std_logic; gt_loopback_in_0 : in std_logic_vector(2 downto 0));\n"
    "  end component;\n";

} // namespace

TEST_CASE("XCI: 10G/25G Ethernet subsystem (xxv_ethernet) PCS/PMA with MII", "[frontend][xci][ethernet]") {
    test::TempDir dir;
    frontend::XciContext ctx;
    ctx.vhdlComponents = frontend::vhdlComponentPorts(pkg(kXxvComponent), "p.vhd");
    auto m = frontend::modelFromXci(dir.write("xxv.xci", xxvXci("xxv")), ctx);
    REQUIRE(m);
    const auto& v = m.value().verilog;
    INFO(v);
    for (const char* p : {"OPENCDC_CLOCK_PIN = \"tx_mii_clk_0\", OPENCDC_CLOCK_PERIOD = 6.4",
                          "OPENCDC_CLOCK_PIN = \"rx_clk_out_0\", OPENCDC_CLOCK_PERIOD = 6.4",
                          "OPENCDC_CLOCK_PIN = \"rxrecclkout_0\" *)",
                          "always @(posedge oc_tx_mii_clk_0_c) oc_dom_tx_mii_clk_0 <= ^{tx_mii_d_0, tx_mii_c_0, "
                          "ctl_tx_test_pattern_0}",
                          "always @(posedge oc_tx_mii_clk_0_c) oc_gt_txbufstatus_0_q",
                          "always @(posedge rx_core_clk_0[0]) oc_rx_mii_d_0_q",
                          "always @(posedge oc_rx_clk_out_0_c) oc_dom_rx_clk_out_0 <= ^{gt_rxpolarity_0, "
                          "ctl_rx_test_pattern_0}",
                          "always @(posedge oc_rx_clk_out_0_c) oc_stat_rx_block_lock_0_q",
                          "always @(posedge dclk[0]) oc_gt_reset_tx_done_out_0_q",
                          "always @(posedge gt_drpclk_0[0]) oc_gt_drpdo_0_q",
                          "opencdc_async_source u_oc_gtpowergood_out_0"})
        CHECK(v.find(p) != std::string::npos);
    // negative: resets and the loopback control are read by no register
    for (const char* p : {"rx_reset_0", "tx_reset_0", "sys_reset", "gt_loopback_in_0"})
        CHECK(v.find(std::string("{") + p) == std::string::npos);
    CHECK(m.value().summary.find("tx_mii_clk/rx_clk_out 156.25 MHz") != std::string::npos);
    // negative: the MAC and several cores are not modelled
    auto mac = frontend::modelFromXci(dir.write("xm.xci", xxvXci("xxv", "Ethernet MAC+PCS/PMA 64-bit")), ctx);
    REQUIRE_FALSE(mac);
    CHECK(mac.error().message.find("only the Ethernet PCS/PMA 64-bit core") != std::string::npos);
    auto four = frontend::modelFromXci(dir.write("x4.xci", xxvXci("xxv", "Ethernet PCS/PMA 64-bit", "4")), ctx);
    REQUIRE_FALSE(four);
    CHECK(four.error().message.find("C_NUM_OF_CORES 4") != std::string::npos);
}

namespace {

std::string gigXci(const std::string& inst, std::map<std::string, std::string> over = {}) {
    std::map<std::string, std::string> p = {{"c_family", "artix7"},        {"c_use_transceiver", "true"},
                                            {"c_use_lvds", "false"},       {"c_use_tbi", "false"},
                                            {"c_support_level", "false"},  {"c_is_sgmii", "false"},
                                            {"c_is_2_5g", "false"},        {"c_rx_gmii_clk_src", "TXOUTCLK"},
                                            {"c_transceiver_type", "GTPE2"}, {"c_num_of_lanes", "1"}};
    for (auto& [k, v] : over)
        p[k] = v;
    return spirit(inst, "gig_ethernet_pcs_pma", "16.0", {p.begin(), p.end()});
}

// SURF GigEthGtp7Core (gig_ethernet_pcs_pma 16.0, 1000BASE-X over a GTP, no shared logic), a part of the ports
const char* kGigGtpComponent =
    "  component gig\n    port (\n"
    "      gtrefclk : in std_logic; gtrefclk_bufg : in std_logic; txp : out std_logic; rxp : in std_logic;\n"
    "      resetdone : out std_logic; cplllock : out std_logic; mmcm_reset : out std_logic;\n"
    "      txoutclk : out std_logic; rxoutclk : out std_logic; userclk : in std_logic; userclk2 : in std_logic;\n"
    "      rxuserclk : in std_logic; rxuserclk2 : in std_logic; pma_reset : in std_logic; mmcm_locked : in std_logic;\n"
    "      independent_clock_bufg : in std_logic; gmii_txd : in std_logic_vector(7 downto 0);\n"
    "      gmii_tx_en : in std_logic; gmii_rxd : out std_logic_vector(7 downto 0); gmii_rx_dv : out std_logic;\n"
    "      configuration_vector : in std_logic_vector(4 downto 0); an_interrupt : out std_logic;\n"
    "      an_restart_config : in std_logic; status_vector : out std_logic_vector(15 downto 0); reset : in std_logic;\n"
    "      gt0_rxbufstatus_out : out std_logic_vector(2 downto 0); gt0_dmonitorout_out : out std_logic_vector(14 downto 0);\n"
    "      gt0_drpclk_in : in std_logic; gt0_drpdo_out : out std_logic_vector(15 downto 0); gt0_drpen_in : in std_logic;\n"
    "      gt0_rxcharisk_out : out std_logic_vector(1 downto 0); gt0_txpolarity_in : in std_logic;\n"
    "      gt0_txdiffctrl_in : in std_logic_vector(3 downto 0); gt0_rxpolarity_in : in std_logic;\n"
    "      gt0_txresetdone_out : out std_logic; gt0_loopback_in : in std_logic_vector(2 downto 0);\n"
    "      signal_detect : in std_logic; gt0_pll0outclk_in : in std_logic; gt0_pll0lock_in : in std_logic;\n"
    "      gt0_pll0reset_out : out std_logic);\n  end component;\n";

// SURF GigEthLvdsUltraScaleCore (gig_ethernet_pcs_pma 16.2, SGMII over UltraScale LVDS, shared logic)
const char* kGigLvdsComponent =
    "  component gl\n    port (\n"
    "      txn : out std_logic; txp : out std_logic; rxn : in std_logic; rxp : in std_logic;\n"
    "      mmcm_locked_out : out std_logic; sgmii_clk_r : out std_logic; sgmii_clk_f : out std_logic;\n"
    "      sgmii_clk_en : out std_logic; clk125_out : out std_logic; clk625_out : out std_logic;\n"
    "      clk312_out : out std_logic; rst_125_out : out std_logic; refclk625_n : in std_logic;\n"
    "      refclk625_p : in std_logic; gmii_txd : in std_logic_vector(7 downto 0); gmii_tx_en : in std_logic;\n"
    "      gmii_tx_er : in std_logic; gmii_rxd : out std_logic_vector(7 downto 0); gmii_rx_dv : out std_logic;\n"
    "      gmii_rx_er : out std_logic; gmii_isolate : out std_logic;\n"
    "      configuration_vector : in std_logic_vector(4 downto 0); speed_is_10_100 : in std_logic;\n"
    "      speed_is_100 : in std_logic; status_vector : out std_logic_vector(15 downto 0); reset : in std_logic;\n"
    "      signal_detect : in std_logic; idelay_rdy_out : out std_logic);\n  end component;\n";

std::map<std::string, std::string> lvdsParams() {
    return {{"c_family", "kintexu"}, {"c_use_transceiver", "false"}, {"c_use_lvds", "true"},
            {"c_support_level", "true"}, {"c_is_sgmii", "true"}};
}

} // namespace

TEST_CASE("XCI: 1G Ethernet PCS/PMA (gig_ethernet_pcs_pma) over a transceiver", "[frontend][xci][ethernet]") {
    test::TempDir dir;
    frontend::XciContext ctx;
    ctx.vhdlComponents = frontend::vhdlComponentPorts(pkg(kGigGtpComponent), "p.vhd");
    auto m = frontend::modelFromXci(dir.write("gig.xci", gigXci("gig")), ctx);
    REQUIRE(m);
    const auto& v = m.value().verilog;
    INFO(v);
    for (const char* p : {"OPENCDC_CLOCK_PIN = \"txoutclk\", OPENCDC_CLOCK_PERIOD = 16",
                          "OPENCDC_CLOCK_PIN = \"rxoutclk\", OPENCDC_CLOCK_PERIOD = 16",
                          "always @(posedge userclk2[0]) oc_dom_userclk2 <= ^{gmii_txd, gmii_tx_en, "
                          "configuration_vector, an_restart_config}",
                          "always @(posedge userclk2[0]) oc_gmii_rxd_q", "always @(posedge userclk2[0]) oc_status_vector_q",
                          "always @(posedge userclk2[0]) oc_an_interrupt_q",
                          "always @(posedge userclk[0]) oc_dom_userclk <= ^{gt0_txpolarity_in}",
                          "always @(posedge userclk[0]) oc_gt0_txresetdone_out_q",
                          "always @(posedge rxuserclk2[0]) oc_dom_rxuserclk2 <= ^{gt0_rxpolarity_in}",
                          "always @(posedge rxuserclk2[0]) oc_gt0_rxcharisk_out_q",
                          "always @(posedge gt0_drpclk_in[0]) oc_gt0_drpdo_out_q",
                          "opencdc_async_source u_oc_resetdone", "opencdc_async_source u_oc_gt0_dmonitorout_out",
                          "opencdc_async_source u_oc_gt0_pll0reset_out", "assign txp = 1'b0;"})
        CHECK(v.find(p) != std::string::npos);
    // negative: resets, signal_detect, the static transceiver controls are read by no register
    for (const char* p : {"reset", "pma_reset", "signal_detect", "gt0_txdiffctrl_in", "gt0_loopback_in", "mmcm_locked"})
        CHECK(v.find(std::string(", ") + p + "}") == std::string::npos);
    CHECK(v.find("{reset") == std::string::npos);
    CHECK(m.value().summary.find("1000BASE-X, GTPE2, line rate 1.25 Gb/s, TXOUTCLK/RXOUTCLK 62.5 MHz, GMII in userclk2") !=
          std::string::npos);

    // GMII RX from RXOUTCLK: in rxuserclk2; 2.5G: 156.25 MHz transceiver clocks
    auto rx = frontend::modelFromXci(
        dir.write("gr.xci", gigXci("gig", {{"c_rx_gmii_clk_src", "RXOUTCLK"}, {"c_is_2_5g", "true"}})), ctx);
    REQUIRE(rx);
    CHECK(rx.value().verilog.find("always @(posedge rxuserclk2[0]) oc_gmii_rxd_q") != std::string::npos);
    CHECK(rx.value().verilog.find("always @(posedge userclk2[0]) oc_gmii_rxd_q") == std::string::npos);
    CHECK(rx.value().verilog.find("OPENCDC_CLOCK_PIN = \"txoutclk\", OPENCDC_CLOCK_PERIOD = 6.4") != std::string::npos);
    CHECK(rx.value().summary.find("2500BASE-X") != std::string::npos);
}

TEST_CASE("XCI: 1G Ethernet PCS/PMA (gig_ethernet_pcs_pma) SGMII over UltraScale LVDS", "[frontend][xci][ethernet]") {
    test::TempDir dir;
    frontend::XciContext ctx;
    ctx.vhdlComponents = frontend::vhdlComponentPorts(pkg(kGigLvdsComponent), "p.vhd");
    auto m = frontend::modelFromXci(dir.write("gl.xci", gigXci("gl", lvdsParams())), ctx);
    REQUIRE(m);
    const auto& v = m.value().verilog;
    INFO(v);
    // the MMCM's outputs are dividers of refclk625: related to it, periods from the reference
    for (const char* p : {"OPENCDC_CLOCK_PIN = \"clk125_out\", OPENCDC_CLOCK_RATIO = 5.0, OPENCDC_CLOCK_INPUT_PERIOD = 1.6 *)",
                          "OPENCDC_CLOCK_PIN = \"clk312_out\", OPENCDC_CLOCK_RATIO = 2.0, OPENCDC_CLOCK_INPUT_PERIOD = 1.6 *)",
                          "OPENCDC_CLOCK_PIN = \"clk625_out\", OPENCDC_CLOCK_RATIO = 1.0, OPENCDC_CLOCK_INPUT_PERIOD = 1.6 *)",
                          "always @(posedge refclk625_p[0]) oc_clk125_out_d <= ~oc_clk125_out_d;",
                          "always @(posedge oc_clk125_out_d) oc_dom_clk125_out <= ^{gmii_txd, gmii_tx_en, gmii_tx_er, "
                          "configuration_vector, speed_is_10_100, speed_is_100}",
                          "always @(posedge oc_clk125_out_d) oc_gmii_rxd_q", "always @(posedge oc_clk125_out_d) oc_sgmii_clk_en_q",
                          "always @(posedge oc_clk125_out_d) oc_rst_125_out_q",
                          "opencdc_async_source u_oc_mmcm_locked_out", "opencdc_async_source u_oc_idelay_rdy_out"})
        CHECK(v.find(p) != std::string::npos);
    // negative: refclk625_n, reset and signal_detect are read by no register; no independent clock source
    for (const char* p : {"refclk625_n", "reset}", "signal_detect"})
        CHECK(v.find(std::string("^{") + p) == std::string::npos);
    CHECK(v.find("OPENCDC_CLOCK_PIN = \"clk125_out\", OPENCDC_CLOCK_PERIOD") == std::string::npos);
    CHECK(m.value().summary.find("SGMII over UltraScale LVDS") != std::string::npos);
}

TEST_CASE("XCI negative: gig_ethernet_pcs_pma configurations that are not modelled", "[frontend][xci][ethernet]") {
    test::TempDir dir;
    frontend::XciContext ctx;
    ctx.vhdlComponents = frontend::vhdlComponentPorts(pkg(kGigGtpComponent), "p.vhd");
    auto expect = [&](std::map<std::string, std::string> over, const std::string& what) {
        auto r = frontend::modelFromXci(dir.write("gig.xci", gigXci("gig", std::move(over))), ctx);
        REQUIRE_FALSE(r);
        INFO(r.error().message);
        CHECK(r.error().message.find(what) != std::string::npos);
    };
    expect({{"c_use_tbi", "true"}, {"c_use_transceiver", "false"}}, "ten-bit interface (TBI)");
    expect({{"c_support_level", "true"}}, "shared logic in the core (c_support_level true)");
    expect({{"c_enable_async_sgmii", "true"}}, "asynchronous SGMII over LVDS");
    expect({{"c_num_of_lanes", "2"}}, "c_num_of_lanes 2");
    auto l7 = lvdsParams();
    l7["c_family"] = "kintex7";
    expect(l7, "UltraScale with the shared logic in the core only");
    // MDIO ports, and an LVDS derived clock without its reference port
    frontend::XciContext odd;
    odd.vhdlComponents = frontend::vhdlComponentPorts(
        pkg("  component gig port (userclk2 : in std_logic; mdc : in std_logic; mdio_i : in std_logic); end component;\n"),
        "p.vhd");
    auto md = frontend::modelFromXci(dir.write("gm.xci", gigXci("gig")), odd);
    REQUIRE_FALSE(md);
    CHECK(md.error().message.find("mdc, mdio_i") != std::string::npos);
    frontend::XciContext noRef;
    noRef.vhdlComponents = frontend::vhdlComponentPorts(
        pkg("  component gl port (clk125_out : out std_logic; gmii_txd : in std_logic_vector(7 downto 0)); end component;\n"),
        "p.vhd");
    auto nr = frontend::modelFromXci(dir.write("gn.xci", gigXci("gl", lvdsParams())), noRef);
    REQUIRE_FALSE(nr);
    CHECK(nr.error().message.find("clk125_out (from refclk625_p)") != std::string::npos);
}

namespace {

std::string gigLvdsTop(const std::string& readClk) {
    return std::string("library ieee;\nuse ieee.std_logic_1164.all;\n"
                       "entity top is\n  port (refp, refn, clk_a, rxp, rxn : in std_logic; q : out std_logic_vector(7 downto 0));\n"
                       "end;\narchitecture rtl of top is\n") +
           kGigLvdsComponent +
           "  signal c125, c312 : std_logic;\n  signal rxd, r : std_logic_vector(7 downto 0);\nbegin\n"
           "  u : gl port map (txn => open, txp => open, rxn => rxn, rxp => rxp, mmcm_locked_out => open,\n"
           "    sgmii_clk_r => open, sgmii_clk_f => open, sgmii_clk_en => open, clk125_out => c125,\n"
           "    clk625_out => open, clk312_out => c312, rst_125_out => open, refclk625_n => refn, refclk625_p => refp,\n"
           "    gmii_txd => (others => '0'), gmii_tx_en => '0', gmii_tx_er => '0', gmii_rxd => rxd, gmii_rx_dv => open,\n"
           "    gmii_rx_er => open, gmii_isolate => open, configuration_vector => (others => '0'),\n"
           "    speed_is_10_100 => '0', speed_is_100 => '0', status_vector => open, reset => '0',\n"
           "    signal_detect => '1', idelay_rdy_out => open);\n"
           "  process (c125) begin if rising_edge(c125) then r <= rxd; end if; end process;\n"
           "  process (" + readClk + ") begin if rising_edge(" + readClk + ") then q <= r; end if; end process;\n"
           "end;\n";
}

} // namespace

TEST_CASE("XCI: gig_ethernet_pcs_pma LVDS clocks derived from the reference in a VHDL design",
          "[frontend][xci][ethernet]") {
    auto e2e = [](const std::string& readClk, const std::string& sdc) {
        test::TempDir dir;
        engine::ProjectConfig cfg;
        cfg.name = "gl";
        cfg.top = "top";
        cfg.sources = {dir.write("top.vhd", gigLvdsTop(readClk)), dir.write("gl.xci", gigXci("gl", lvdsParams()))};
        cfg.sourceLibraries = {"", ""};
        cfg.constraints = {dir.write("c.sdc", sdc)};
        return run(cfg);
    };
    const std::string base = "create_clock -name ref -period 1.6 [get_ports refp]\n"
                             "create_clock -name a -period 10 [get_ports clk_a]\n"
                             "set_clock_groups -asynchronous -group [get_clocks -include_generated_clocks ref] -group a\n";
    // 125 MHz to 312.5 MHz: both from the IP's MMCM, related, no unsynchronized crossing
    auto rel = e2e("c312", base);
    REQUIRE(rel->cdc);
    CHECK(count(*rel, "FE-XCI-MODEL") == 1);
    CHECK(rel->cdc->stats.unsynchronized == 0);
    CHECK(rel->cdc->stats.unknownRelation == 0);
    REQUIRE(rel->clocks);
    int found = 0;
    for (const auto& c : rel->clocks->clocks) {
        if (c.name.ends_with("u.clk125_out") && c.periodNs) {
            CHECK((*c.periodNs > 7.99 && *c.periodNs < 8.01));
            ++found;
        }
        if (c.name.ends_with("u.clk312_out") && c.periodNs) {
            CHECK((*c.periodNs > 3.19 && *c.periodNs < 3.21));
            ++found;
        }
    }
    CHECK(found == 2);
    // negative: into a clock asynchronous to the reference, the crossing is reported
    auto other = e2e("clk_a", base);
    REQUIRE(other->cdc);
    CHECK(other->cdc->stats.unsynchronized == 1);
    CHECK(other->cdc->stats.unknownRelation == 0);
}

namespace {

std::string xauiXci(const std::string& inst, const std::string& dxaui = "false") {
    return spirit(inst, "xaui", "12.3", {{"c_family", "kintexu"}, {"c_is_dxaui", dxaui}, {"c_has_mdio", "false"}});
}

// SURF XauiGthUltraScale156p25MHz10GigECore (xaui 12.3, shared logic in the core), a part of the ports
const char* kXauiComponent =
    "  component xa\n    port (\n"
    "      dclk : in std_logic; reset : in std_logic; clk156_out : out std_logic; clk156_lock : out std_logic;\n"
    "      refclk : in std_logic; xgmii_txd : in std_logic_vector(63 downto 0); xgmii_txc : in std_logic_vector(7 downto 0);\n"
    "      xgmii_rxd : out std_logic_vector(63 downto 0); xgmii_rxc : out std_logic_vector(7 downto 0);\n"
    "      xaui_tx_l0_p : out std_logic; xaui_rx_l0_p : in std_logic; signal_detect : in std_logic_vector(3 downto 0);\n"
    "      debug : out std_logic_vector(5 downto 0); gt0_drpaddr : in std_logic_vector(9 downto 0);\n"
    "      gt0_drpen : in std_logic; gt0_drpdo : out std_logic_vector(15 downto 0); gt3_drprdy : out std_logic;\n"
    "      gt_reset_tx_datapath : in std_logic; gt_txresetdone : out std_logic_vector(3 downto 0);\n"
    "      gt_rxpmaresetdone : out std_logic_vector(3 downto 0); gt_rxbufstatus : out std_logic_vector(11 downto 0);\n"
    "      gt_qplllock : out std_logic; gt_loopback : in std_logic_vector(11 downto 0);\n"
    "      gt_rxpolarity : in std_logic_vector(3 downto 0); gt_txdiffctrl : in std_logic_vector(19 downto 0);\n"
    "      gt_dmonitorout : out std_logic_vector(63 downto 0); gt_rxdisperr : out std_logic_vector(7 downto 0);\n"
    "      gt_powergood_out : out std_logic_vector(3 downto 0);\n"
    "      configuration_vector : in std_logic_vector(6 downto 0); status_vector : out std_logic_vector(7 downto 0));\n"
    "  end component;\n";

} // namespace

TEST_CASE("XCI: XAUI (xaui) with the shared logic: everything in clk156_out", "[frontend][xci][ethernet]") {
    test::TempDir dir;
    frontend::XciContext ctx;
    ctx.vhdlComponents = frontend::vhdlComponentPorts(pkg(kXauiComponent), "p.vhd");
    auto m = frontend::modelFromXci(dir.write("xa.xci", xauiXci("xa")), ctx);
    REQUIRE(m);
    const auto& v = m.value().verilog;
    INFO(v);
    for (const char* p : {"OPENCDC_CLOCK_PIN = \"clk156_out\", OPENCDC_CLOCK_PERIOD = 6.4",
                          "always @(posedge oc_clk156_out_c) oc_dom_clk156_out <= ^{xgmii_txd, xgmii_txc, gt_rxpolarity, "
                          "configuration_vector}",
                          "always @(posedge oc_clk156_out_c) oc_xgmii_rxd_q", "always @(posedge oc_clk156_out_c) oc_status_vector_q",
                          "always @(posedge oc_clk156_out_c) oc_debug_q", "always @(posedge oc_clk156_out_c) oc_gt_txresetdone_q",
                          "always @(posedge oc_clk156_out_c) oc_gt_rxbufstatus_q",
                          "always @(posedge oc_clk156_out_c) oc_gt_rxdisperr_q",
                          "always @(posedge dclk[0]) oc_dom_dclk <= ^{gt0_drpaddr, gt0_drpen}",
                          "always @(posedge dclk[0]) oc_gt0_drpdo_q", "always @(posedge dclk[0]) oc_gt3_drprdy_q",
                          "opencdc_async_source u_oc_clk156_lock", "opencdc_async_source u_oc_gt_rxpmaresetdone",
                          "opencdc_async_source u_oc_gt_qplllock", "opencdc_async_source u_oc_gt_dmonitorout",
                          "opencdc_async_source u_oc_gt_powergood_out", "assign xaui_tx_l0_p = 1'b0;"})
        CHECK(v.find(p) != std::string::npos);
    // negative: reset, signal_detect, the transceiver resets and static controls are read by no register
    for (const char* p : {"reset", "signal_detect", "gt_reset_tx_datapath", "gt_loopback", "gt_txdiffctrl", "refclk"})
        CHECK(v.find(std::string(", ") + p + ",") == std::string::npos);
    CHECK(v.find("{reset") == std::string::npos);
    CHECK(m.value().summary.find("XAUI, 4 lanes at 3.125 Gb/s, clk156_out 156.25 MHz") != std::string::npos);
    // DXAUI: 6.25 Gb/s lanes, 312.5 MHz
    auto dx = frontend::modelFromXci(dir.write("xd.xci", xauiXci("xa", "true")), ctx);
    REQUIRE(dx);
    CHECK(dx.value().verilog.find("OPENCDC_CLOCK_PIN = \"clk156_out\", OPENCDC_CLOCK_PERIOD = 3.2") != std::string::npos);
    // negative: the core without the shared logic, MDIO ports
    frontend::XciContext core;
    core.vhdlComponents = frontend::vhdlComponentPorts(
        pkg("  component xa port (usrclk : in std_logic; xgmii_txd : in std_logic_vector(63 downto 0)); end component;\n"),
        "p.vhd");
    auto nc = frontend::modelFromXci(dir.write("xn.xci", xauiXci("xa")), core);
    REQUIRE_FALSE(nc);
    CHECK(nc.error().message.find("only the core with the shared logic") != std::string::npos);
    frontend::XciContext md;
    md.vhdlComponents = frontend::vhdlComponentPorts(
        pkg("  component xa port (clk156_out : out std_logic; mdc : in std_logic; mdio_in : in std_logic); end component;\n"),
        "p.vhd");
    auto mm = frontend::modelFromXci(dir.write("xm.xci", xauiXci("xa")), md);
    REQUIRE_FALSE(mm);
    CHECK(mm.error().message.find("mdc, mdio_in") != std::string::npos);
}

namespace {

std::string cmacXci(const std::string& inst, std::map<std::string, std::string> over = {}) {
    std::map<std::string, std::string> p = {{"C_USER_INTERFACE", "AXIS"}, {"C_CMAC_CAUI4_MODE", "1"},
                                            {"C_NUM_LANES", "4"},         {"C_LINE_RATE", "25.78125"},
                                            {"C_GT_REF_CLK_FREQ", "161.1328125"}, {"C_CLOCKING_MODE", "Asynchronous"},
                                            {"C_INCLUDE_SHARED_LOGIC", "2"}};
    for (auto& [k, v] : over)
        p[k] = v;
    return spirit(inst, "cmac_usplus", "3.1", {p.begin(), p.end()});
}

// SURF Caui4GtyIpCore161MHz (cmac_usplus 3.1, CAUI-4, AXI4-Stream, shared logic in the core), a part of the ports
const char* kCmacComponent =
    "  component cm\n    port (\n"
    "      gt_txp_out : out std_logic_vector(3 downto 0); gt_rxp_in : in std_logic_vector(3 downto 0);\n"
    "      gt_txusrclk2 : out std_logic; gt_loopback_in : in std_logic_vector(11 downto 0);\n"
    "      gt_rxpolarity : in std_logic_vector(3 downto 0); gt_txpolarity : in std_logic_vector(3 downto 0);\n"
    "      gt_txdiffctrl : in std_logic_vector(19 downto 0); gt_eyescandataerror : out std_logic_vector(3 downto 0);\n"
    "      gt_ref_clk_out : out std_logic; gt_rxrecclkout : out std_logic_vector(3 downto 0);\n"
    "      gt_powergoodout : out std_logic_vector(3 downto 0); gt_txbufstatus : out std_logic_vector(7 downto 0);\n"
    "      gt_rxresetdone : out std_logic_vector(3 downto 0); gtwiz_reset_tx_datapath : in std_logic;\n"
    "      gt_drpclk : in std_logic; gt0_drpdo : out std_logic_vector(15 downto 0); gt0_drpen : in std_logic;\n"
    "      ctl_tx_rsfec_enable : in std_logic; ctl_rx_rsfec_enable : in std_logic;\n"
    "      stat_rx_rsfec_hi_ser : out std_logic; sys_reset : in std_logic; gt_ref_clk_p : in std_logic;\n"
    "      gt_ref_clk_n : in std_logic; init_clk : in std_logic; common0_drpen : in std_logic;\n"
    "      common0_drpdo : out std_logic_vector(15 downto 0); rx_axis_tvalid : out std_logic;\n"
    "      rx_axis_tdata : out std_logic_vector(511 downto 0); rx_otn_ena : out std_logic;\n"
    "      usr_rx_reset : out std_logic; gt_rxusrclk2 : out std_logic; stat_rx_aligned : out std_logic;\n"
    "      ctl_rx_enable : in std_logic; core_rx_reset : in std_logic; rx_clk : in std_logic;\n"
    "      stat_tx_local_fault : out std_logic; ctl_tx_enable : in std_logic; core_tx_reset : in std_logic;\n"
    "      tx_axis_tready : out std_logic; tx_axis_tvalid : in std_logic; tx_axis_tdata : in std_logic_vector(511 downto 0);\n"
    "      tx_ovfout : out std_logic; usr_tx_reset : out std_logic; core_drp_reset : in std_logic; drp_clk : in std_logic;\n"
    "      drp_en : in std_logic; drp_do : out std_logic_vector(15 downto 0));\n  end component;\n";

} // namespace

TEST_CASE("XCI: 100G Ethernet (cmac_usplus) CAUI-4 with AXI4-Stream", "[frontend][xci][ethernet]") {
    test::TempDir dir;
    frontend::XciContext ctx;
    ctx.vhdlComponents = frontend::vhdlComponentPorts(pkg(kCmacComponent), "p.vhd");
    auto m = frontend::modelFromXci(dir.write("cm.xci", cmacXci("cm")), ctx);
    REQUIRE(m);
    const auto& v = m.value().verilog;
    INFO(v);
    for (const char* p : {"OPENCDC_CLOCK_PIN = \"gt_txusrclk2\", OPENCDC_CLOCK_PERIOD = 3.103030303",
                          "OPENCDC_CLOCK_PIN = \"gt_rxusrclk2\", OPENCDC_CLOCK_PERIOD = 3.103030303",
                          "OPENCDC_CLOCK_PIN = \"gt_rxrecclkout\" *)",
                          // the reference clock through the IP: derived from gt_ref_clk_p, ratio 1
                          "OPENCDC_CLOCK_PIN = \"gt_ref_clk_out\", OPENCDC_CLOCK_RATIO = 1.0, "
                          "OPENCDC_CLOCK_INPUT_PERIOD = 6.206060606",
                          "always @(posedge gt_ref_clk_p[0]) oc_gt_ref_clk_out_d",
                          "always @(posedge oc_gt_txusrclk2_c) oc_dom_gt_txusrclk2 <= ^{gt_txpolarity, "
                          "ctl_tx_rsfec_enable, ctl_tx_enable, tx_axis_tvalid, tx_axis_tdata}",
                          "always @(posedge oc_gt_txusrclk2_c) oc_tx_axis_tready_q",
                          "always @(posedge oc_gt_txusrclk2_c) oc_usr_tx_reset_q",
                          "always @(posedge oc_gt_txusrclk2_c) oc_stat_tx_local_fault_q",
                          "always @(posedge oc_gt_txusrclk2_c) oc_gt_txbufstatus_q",
                          "always @(posedge rx_clk[0]) oc_dom_rx_clk <= ^{ctl_rx_rsfec_enable, ctl_rx_enable}",
                          "always @(posedge rx_clk[0]) oc_rx_axis_tdata_q", "always @(posedge rx_clk[0]) oc_usr_rx_reset_q",
                          "always @(posedge rx_clk[0]) oc_stat_rx_aligned_q", "always @(posedge rx_clk[0]) oc_rx_otn_ena_q",
                          "always @(posedge oc_gt_rxusrclk2_c) oc_gt_rxresetdone_q",
                          "always @(posedge gt_drpclk[0]) oc_gt0_drpdo_q", "always @(posedge gt_drpclk[0]) oc_common0_drpdo_q",
                          "always @(posedge drp_clk[0]) oc_drp_do_q",
                          "opencdc_async_source u_oc_gt_eyescandataerror", "opencdc_async_source u_oc_gt_powergoodout"})
        CHECK(v.find(p) != std::string::npos);
    // negative: resets, the reference's negative pin and static transceiver controls are read by no register
    for (const char* p : {"sys_reset", "core_rx_reset", "core_tx_reset", "core_drp_reset", "gtwiz_reset_tx_datapath",
                          "gt_ref_clk_n", "gt_loopback_in", "gt_txdiffctrl"}) {
        const std::string n = p;
        CHECK(v.find("{" + n) == std::string::npos);
        CHECK(v.find(", " + n + ",") == std::string::npos);
        CHECK(v.find(", " + n + "}") == std::string::npos);
    }
    CHECK(m.value().summary.find("gt_txusrclk2/gt_rxusrclk2 322.265625 MHz, gt_ref_clk_out 161.1328125 MHz") !=
          std::string::npos);
    // negative: LBUS, CAUI-10, ports that are not modelled (auto-negotiation)
    auto lbus = frontend::modelFromXci(dir.write("cl.xci", cmacXci("cm", {{"C_USER_INTERFACE", "LBUS"}})), ctx);
    REQUIRE_FALSE(lbus);
    CHECK(lbus.error().message.find("only the AXI4-Stream user interface") != std::string::npos);
    auto c10 = frontend::modelFromXci(
        dir.write("c10.xci", cmacXci("cm", {{"C_CMAC_CAUI4_MODE", "0"}, {"C_NUM_LANES", "10"}})), ctx);
    REQUIRE_FALSE(c10);
    CHECK(c10.error().message.find("only CAUI-4") != std::string::npos);
    frontend::XciContext an;
    an.vhdlComponents = frontend::vhdlComponentPorts(
        pkg("  component cm port (gt_txusrclk2 : out std_logic; an_clk : in std_logic; ctl_autoneg_enable : in std_logic); "
            "end component;\n"),
        "p.vhd");
    auto ma = frontend::modelFromXci(dir.write("ca.xci", cmacXci("cm")), an);
    REQUIRE_FALSE(ma);
    CHECK(ma.error().message.find("an_clk, ctl_autoneg_enable") != std::string::npos);
}

namespace {

// SURF Pgp4GtyUsIpFec (ieee802d3_clause74_fec 1.0): a JSON .xci, the ports in its boundary
std::string jsonFec(const std::vector<std::string>& extra = {}) {
    std::string ports;
    auto add = [&](const std::string& p) { ports += (ports.empty() ? "" : ",\n        ") + p; };
    for (const char* n : {"tx_clk", "tx_reset", "rx_clk", "rx_reset", "tx_din_start", "ctrl_rx_indication_en"})
        add(port(n, "in"));
    add(port("tx_din", "in", 65));
    add(port("rx_din", "in", 65));
    add(port("ctrl_rx_header_mark", "in", 31));
    add(port("tx_dout", "out", 65));
    add(port("rx_dout", "out", 65));
    for (const char* n : {"tx_dout_start", "rx_din_slip", "rx_dout_start", "stat_rx_aligned", "stat_rx_cw_corrected",
                          "stat_rx_cw_uncorrected", "stat_rx_cw_inc"})
        add(port(n, "out"));
    for (const auto& e : extra)
        add(port(e, "in"));
    return "{\n  \"schema\": \"xilinx.com:schema:json_instance:1.0\",\n  \"ip_inst\": {\n"
           "    \"xci_name\": \"fec\",\n"
           "    \"component_reference\": \"xilinx.com:ip:ieee802d3_clause74_fec:1.0\",\n"
           "    \"ip_revision\": \"20\",\n"
           "    \"parameters\": {\n      \"component_parameters\": {\n        " + param("Component_Name", "fec") +
           "\n      }\n    },\n"
           "    \"boundary\": {\n      \"ports\": {\n        " + ports + "\n      }\n    }\n  }\n}\n";
}

} // namespace

TEST_CASE("XCI: IEEE 802.3 Clause 74 FEC (ieee802d3_clause74_fec), encoder and decoder clocks",
          "[frontend][xci][ethernet]") {
    test::TempDir dir;
    auto m = frontend::modelFromXci(dir.write("fec.xci", jsonFec()), frontend::XciContext{});
    REQUIRE(m);
    const auto& v = m.value().verilog;
    INFO(v);
    for (const char* p : {"always @(posedge tx_clk[0]) oc_dom_tx_clk <= ^{tx_din, tx_din_start, tx_reset}",
                          "always @(posedge tx_clk[0]) oc_tx_dout_q", "always @(posedge tx_clk[0]) oc_tx_dout_start_q",
                          "always @(posedge rx_clk[0]) oc_dom_rx_clk <= ^{ctrl_rx_header_mark, ctrl_rx_indication_en, "
                          "rx_din, rx_reset}",
                          "always @(posedge rx_clk[0]) oc_rx_dout_q", "always @(posedge rx_clk[0]) oc_rx_din_slip_q",
                          "always @(posedge rx_clk[0]) oc_stat_rx_aligned_q"})
        CHECK(v.find(p) != std::string::npos);
    // negative: nothing crosses inside the IP, no transmit port in the receive clock or the other way round
    CHECK(v.find("posedge rx_clk[0]) oc_tx_") == std::string::npos);
    CHECK(v.find("posedge tx_clk[0]) oc_rx_") == std::string::npos);
    CHECK(v.find("opencdc_async_source") == std::string::npos);
    // negative: an unknown port is reported
    auto odd = frontend::modelFromXci(dir.write("fo.xci", jsonFec({"core_clk"})), frontend::XciContext{});
    REQUIRE_FALSE(odd);
    CHECK(odd.error().message.find("core_clk") != std::string::npos);
}

namespace {

std::string dmaXci(const std::string& inst, const std::string& async, const std::string& sg = "0") {
    return spirit(inst, "axi_dma", "7.1",
                  {{"C_PRMRY_IS_ACLK_ASYNC", async}, {"C_INCLUDE_SG", sg}, {"C_ENABLE_MULTI_CHANNEL", "0"},
                   {"C_INCLUDE_MM2S", "1"}, {"C_INCLUDE_S2MM", "1"}});
}

// Digilent usb2device's axi_dma_0 (axi_dma 7.1, direct register mode), a part of the ports
const char* kDmaComponent =
    "  component dma\n    port (\n"
    "      s_axi_lite_aclk : in std_logic; m_axi_mm2s_aclk : in std_logic; m_axi_s2mm_aclk : in std_logic;\n"
    "      axi_resetn : in std_logic; s_axi_lite_awvalid : in std_logic; s_axi_lite_awready : out std_logic;\n"
    "      s_axi_lite_rdata : out std_logic_vector(31 downto 0); m_axi_mm2s_araddr : out std_logic_vector(31 downto 0);\n"
    "      m_axi_mm2s_rdata : in std_logic_vector(31 downto 0); mm2s_prmry_reset_out_n : out std_logic;\n"
    "      m_axis_mm2s_tdata : out std_logic_vector(31 downto 0); m_axis_mm2s_tready : in std_logic;\n"
    "      m_axi_s2mm_awaddr : out std_logic_vector(31 downto 0); m_axi_s2mm_bvalid : in std_logic;\n"
    "      s2mm_prmry_reset_out_n : out std_logic; s_axis_s2mm_tdata : in std_logic_vector(31 downto 0);\n"
    "      s_axis_s2mm_tready : out std_logic; mm2s_introut : out std_logic; s2mm_introut : out std_logic;\n"
    "      axi_dma_tstvec : out std_logic_vector(31 downto 0));\n  end component;\n";

} // namespace

TEST_CASE("XCI: AXI DMA (axi_dma) in direct register mode, synchronous and asynchronous clocks",
          "[frontend][xci][dma]") {
    test::TempDir dir;
    frontend::XciContext ctx;
    ctx.vhdlComponents = frontend::vhdlComponentPorts(pkg(kDmaComponent), "p.vhd");
    // asynchronous clocks: a domain per channel
    auto a = frontend::modelFromXci(dir.write("da.xci", dmaXci("dma", "1")), ctx);
    REQUIRE(a);
    const auto& va = a.value().verilog;
    INFO(va);
    for (const char* p : {"always @(posedge s_axi_lite_aclk[0]) oc_dom_s_axi_lite_aclk <= ^{axi_resetn, s_axi_lite_awvalid}",
                          "always @(posedge s_axi_lite_aclk[0]) oc_s_axi_lite_rdata_q",
                          "always @(posedge s_axi_lite_aclk[0]) oc_mm2s_introut_q",
                          "always @(posedge s_axi_lite_aclk[0]) oc_s2mm_introut_q",
                          "always @(posedge m_axi_mm2s_aclk[0]) oc_dom_m_axi_mm2s_aclk <= ^{m_axi_mm2s_rdata, "
                          "m_axis_mm2s_tready}",
                          "always @(posedge m_axi_mm2s_aclk[0]) oc_m_axis_mm2s_tdata_q",
                          "always @(posedge m_axi_mm2s_aclk[0]) oc_mm2s_prmry_reset_out_n_q",
                          "always @(posedge m_axi_s2mm_aclk[0]) oc_dom_m_axi_s2mm_aclk <= ^{m_axi_s2mm_bvalid, "
                          "s_axis_s2mm_tdata}",
                          "always @(posedge m_axi_s2mm_aclk[0]) oc_s_axis_s2mm_tready_q",
                          "always @(posedge m_axi_s2mm_aclk[0]) oc_s2mm_prmry_reset_out_n_q",
                          "opencdc_async_source u_oc_axi_dma_tstvec"})
        CHECK(va.find(p) != std::string::npos);
    CHECK(a.value().summary.find("asynchronous clocks") != std::string::npos);
    // synchronous clocks: PG021 requires one clock, every port in s_axi_lite_aclk
    auto s = frontend::modelFromXci(dir.write("ds.xci", dmaXci("dma", "0")), ctx);
    REQUIRE(s);
    const auto& vs = s.value().verilog;
    INFO(vs);
    CHECK(vs.find("always @(posedge s_axi_lite_aclk[0]) oc_m_axis_mm2s_tdata_q") != std::string::npos);
    CHECK(vs.find("always @(posedge s_axi_lite_aclk[0]) oc_s_axis_s2mm_tready_q") != std::string::npos);
    // negative: no register in the MM2S or S2MM clock in synchronous mode
    CHECK(vs.find("posedge m_axi_mm2s_aclk[0]") == std::string::npos);
    CHECK(vs.find("posedge m_axi_s2mm_aclk[0]") == std::string::npos);
    // negative: scatter gather is not modelled
    auto sg = frontend::modelFromXci(dir.write("dg.xci", dmaXci("dma", "1", "1")), ctx);
    REQUIRE_FALSE(sg);
    CHECK(sg.error().message.find("scatter gather") != std::string::npos);
}

TEST_CASE("XCI: AXI Clock Converter (axi_clock_converter), ports from the configuration", "[frontend][xci][axi]") {
    test::TempDir dir;
    // Loongson NSCSCC soc_axi_func: AXI3, 4-bit IDs, asynchronous clocks
    auto cc = [&](const std::string& protocol) {
        return frontend::modelFromXci(
            dir.write("cc" + protocol + ".xci",
                      spirit("cc", "axi_clock_converter", "2.1",
                             {{"C_AXI_PROTOCOL", protocol}, {"C_AXI_ID_WIDTH", "4"}, {"C_AXI_ADDR_WIDTH", "32"},
                              {"C_AXI_DATA_WIDTH", "32"}, {"C_AXI_IS_ACLK_ASYNC", "1"}, {"C_SYNCHRONIZER_STAGE", "3"},
                              {"C_AXI_SUPPORTS_USER_SIGNALS", "0"}})),
            frontend::XciContext{});
    };
    auto a3 = cc("1");
    REQUIRE(a3);
    const auto& v = a3.value().verilog;
    INFO(v);
    for (const char* p : {"input  wire [3:0] s_axi_awid", "input  wire [3:0] s_axi_awlen", "input  wire [1:0] s_axi_awlock",
                          "input  wire [3:0] s_axi_wid", "input  wire [3:0] s_axi_awqos", "output wire [31:0] m_axi_awaddr",
                          "input  wire [31:0] m_axi_rdata", "output wire [3:0] s_axi_rid",
                          "always @(posedge s_axi_aclk[0]) oc_s_axi_rdata_q", "always @(posedge s_axi_aclk[0]) oc_s_axi_awready_q",
                          "always @(posedge m_axi_aclk[0]) oc_m_axi_awaddr_q", "always @(posedge m_axi_aclk[0]) oc_m_axi_rready_q"})
        CHECK(v.find(p) != std::string::npos);
    // negative: AXI3 has no region, nothing of one side in the other side's clock
    CHECK(v.find("awregion") == std::string::npos);
    CHECK(v.find("posedge m_axi_aclk[0]) oc_s_axi_") == std::string::npos);
    CHECK(v.find("posedge s_axi_aclk[0]) oc_m_axi_") == std::string::npos);
    CHECK(a3.value().summary.find("AXI3, s_axi_aclk to m_axi_aclk, asynchronous clocks (3 synchronizer stages)") !=
          std::string::npos);
    // AXI4: 8-bit lengths, region, no WID; AXI4-Lite: no IDs, no bursts
    auto a4 = cc("0");
    REQUIRE(a4);
    CHECK(a4.value().verilog.find("input  wire [7:0] s_axi_awlen") != std::string::npos);
    CHECK(a4.value().verilog.find("s_axi_awregion") != std::string::npos);
    CHECK(a4.value().verilog.find("s_axi_wid") == std::string::npos);
    auto lite = cc("2");
    REQUIRE(lite);
    CHECK(lite.value().verilog.find("s_axi_awid") == std::string::npos);
    CHECK(lite.value().verilog.find("s_axi_awlen") == std::string::npos);
    CHECK(lite.value().verilog.find("input  wire [31:0] s_axi_awaddr") != std::string::npos);
}

TEST_CASE("XCI: AXI Crossbar (axi_crossbar), one clock, slots concatenated", "[frontend][xci][axi]") {
    test::TempDir dir;
    // soc_axi_func axi_crossbar_1x2: 1 slave slot, 2 master slots, AXI3
    auto m = frontend::modelFromXci(
        dir.write("xb.xci", spirit("xb", "axi_crossbar", "2.1",
                                   {{"C_AXI_PROTOCOL", "1"}, {"C_AXI_ID_WIDTH", "4"}, {"C_AXI_ADDR_WIDTH", "32"},
                                    {"C_AXI_DATA_WIDTH", "32"}, {"C_NUM_SLAVE_SLOTS", "1"}, {"C_NUM_MASTER_SLOTS", "2"},
                                    {"C_AXI_SUPPORTS_USER_SIGNALS", "0"}})),
        frontend::XciContext{});
    REQUIRE(m);
    const auto& v = m.value().verilog;
    INFO(v);
    for (const char* p : {"input  wire [31:0] s_axi_awaddr", "output wire [63:0] m_axi_awaddr", "output wire [1:0] m_axi_awvalid",
                          "input  wire [1:0] m_axi_awready", "input  wire [63:0] m_axi_rdata",
                          "always @(posedge aclk[0]) oc_dom_aclk", "always @(posedge aclk[0]) oc_m_axi_awaddr_q",
                          "always @(posedge aclk[0]) oc_s_axi_rdata_q"})
        CHECK(v.find(p) != std::string::npos);
    CHECK(m.value().summary.find("AXI3, 1 x 2, one clock (aclk)") != std::string::npos);
    // negative: aresetn is read in aclk, there is no other clock
    CHECK(v.find("opencdc_async_source") == std::string::npos);
}

TEST_CASE("XCI: Block Memory Generator with the AXI4 interface, ports from the configuration", "[frontend][xci][bram]") {
    test::TempDir dir;
    auto axi = [&](const std::string& type, const std::string& hasId) {
        return frontend::modelFromXci(
            dir.write("ram" + type + hasId + ".xci",
                      spirit("ram", "blk_mem_gen", "8.4",
                             {{"C_INTERFACE_TYPE", "1"}, {"C_AXI_TYPE", type}, {"C_HAS_AXI_ID", hasId},
                              {"C_AXI_ID_WIDTH", "4"}, {"C_WRITE_WIDTH_A", "32"}, {"C_WRITE_DEPTH_A", "262144"},
                              {"C_MEM_TYPE", "1"}})),
            frontend::XciContext{});
    };
    auto m = axi("1", "1");
    REQUIRE(m);
    const auto& v = m.value().verilog;
    INFO(v);
    for (const char* p : {"input  wire [3:0] s_axi_awid", "input  wire [7:0] s_axi_arlen", "input  wire [3:0] s_axi_wstrb",
                          "output wire [31:0] s_axi_rdata", "output wire [3:0] s_axi_bid",
                          "always @(posedge s_aclk[0]) oc_dom_s_aclk", "always @(posedge s_aclk[0]) oc_s_axi_rdata_q"})
        CHECK(v.find(p) != std::string::npos);
    // negative: no lock/cache/prot ports (PG058), no IDs without C_HAS_AXI_ID, AXI4-Lite without bursts
    CHECK(v.find("s_axi_awlock") == std::string::npos);
    CHECK(v.find("s_axi_arprot") == std::string::npos);
    auto noId = axi("1", "0");
    REQUIRE(noId);
    CHECK(noId.value().verilog.find("s_axi_awid") == std::string::npos);
    auto lite = axi("0", "0");
    REQUIRE(lite);
    CHECK(lite.value().verilog.find("s_axi_awlen") == std::string::npos);
    CHECK(lite.value().verilog.find("s_axi_wlast") == std::string::npos);
    CHECK(lite.value().summary.find("AXI4-Lite slave") != std::string::npos);
}

TEST_CASE("XCI: Floating Point from an XML .xci, ports from the configuration", "[frontend][xci][datapath]") {
    test::TempDir dir;
    // nontrivial-mips floating_point_addsub: A, B and the operation channel, result tuser (3 bits)
    auto m = frontend::modelFromXci(
        dir.write("fp.xci", spirit("fp", "floating_point", "7.1",
                                   {{"C_A_TDATA_WIDTH", "32"}, {"C_HAS_B", "1"}, {"C_B_TDATA_WIDTH", "32"}, {"C_HAS_C", "0"},
                                    {"C_HAS_OPERATION", "1"}, {"C_OPERATION_TDATA_WIDTH", "8"},
                                    {"C_RESULT_TDATA_WIDTH", "32"}, {"C_HAS_RESULT_TUSER", "1"},
                                    {"C_RESULT_TUSER_WIDTH", "3"}, {"C_THROTTLE_SCHEME", "3"},
                                    {"C_HAS_ACLKEN", "0"}, {"C_HAS_ARESETN", "0"}})),
        frontend::XciContext{});
    REQUIRE(m);
    const auto& v = m.value().verilog;
    INFO(v);
    for (const char* p : {"input  wire [31:0] s_axis_a_tdata", "input  wire [31:0] s_axis_b_tdata",
                          "input  wire [7:0] s_axis_operation_tdata", "output wire [2:0] m_axis_result_tuser",
                          "always @(posedge aclk) oc_m_axis_result_tdata"})
        CHECK(v.find(p) != std::string::npos);
    // negative: non-blocking (no tready), no C channel
    CHECK(v.find("tready") == std::string::npos);
    CHECK(v.find("s_axis_c_") == std::string::npos);
}

TEST_CASE("XCI: Clocking Wizard with stale PLL values uses the MMCM values for the frequencies",
          "[frontend][xci][clkwiz]") {
    test::TempDir dir;
    // soc_axi_func clk_pll: C_PRIMITIVE PLL, C_PLL_* all 1, C_MMCM_* give 50 and 100 MHz from 100 MHz
    auto wiz = [&](const std::string& pllMult) {
        return frontend::modelFromXci(
            dir.write("pll" + pllMult + ".xci",
                      spirit("clk_pll", "clk_wiz", "6.0",
                             {{"C_PRIMITIVE", "PLL"}, {"C_PRIM_IN_FREQ", "100.000"}, {"C_CLKOUT2_USED", "1"},
                              {"C_CLK_OUT1_PORT", "cpu_clk"}, {"C_CLK_OUT2_PORT", "sys_clk"},
                              {"C_CLKOUT1_OUT_FREQ", "50.000"}, {"C_CLKOUT2_OUT_FREQ", "100.000"},
                              {"C_PLL_CLKFBOUT_MULT", pllMult}, {"C_PLL_DIVCLK_DIVIDE", "1"},
                              {"C_PLL_CLKOUT0_DIVIDE", pllMult == "1" ? "1" : "16"},
                              {"C_PLL_CLKOUT1_DIVIDE", pllMult == "1" ? "1" : "8"},
                              {"C_MMCM_CLKFBOUT_MULT_F", "9.000"}, {"C_MMCM_DIVCLK_DIVIDE", "1"},
                              {"C_MMCM_CLKOUT0_DIVIDE_F", "18.000"}, {"C_MMCM_CLKOUT1_DIVIDE", "9"},
                              {"C_USE_RESET", "0"}, {"C_USE_LOCKED", "0"}})),
            frontend::XciContext{});
    };
    auto stale = wiz("1");
    REQUIRE(stale);
    INFO(stale.value().verilog);
    CHECK(stale.value().verilog.find("MMCME2_ADV #(.CLKIN1_PERIOD(") != std::string::npos);
    CHECK(stale.value().verilog.find(".CLKFBOUT_MULT_F(9.0)") != std::string::npos);
    CHECK(std::ranges::any_of(stale.value().notes, [](const std::string& n) { return n.find("C_MMCM_*") != std::string::npos; }));
    // negative: consistent PLL values (8 x 100 / 16 = 50, / 8 = 100) are used as they are
    auto good = wiz("8");
    REQUIRE(good);
    CHECK(good.value().verilog.find("PLLE2_ADV #(") != std::string::npos);
    CHECK(good.value().verilog.find(".CLKFBOUT_MULT(8)") != std::string::npos);
    CHECK(good.value().notes.empty());
}

namespace {

/// A ZIP archive of stored entries (no compression, CRC not checked by the reader).
std::string storedZip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, central;
    auto u16 = [](std::string& s, unsigned v) {
        s += static_cast<char>(v & 0xFF);
        s += static_cast<char>((v >> 8) & 0xFF);
    };
    auto u32 = [&](std::string& s, std::uint32_t v) {
        u16(s, v & 0xFFFF);
        u16(s, v >> 16);
    };
    for (const auto& [name, data] : files) {
        const auto offset = static_cast<std::uint32_t>(out.size());
        u32(out, 0x04034b50);
        u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0);
        u32(out, 0); u32(out, static_cast<std::uint32_t>(data.size())); u32(out, static_cast<std::uint32_t>(data.size()));
        u16(out, static_cast<unsigned>(name.size())); u16(out, 0);
        out += name;
        out += data;
        u32(central, 0x02014b50);
        u16(central, 20); u16(central, 20); u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0);
        u32(central, 0); u32(central, static_cast<std::uint32_t>(data.size()));
        u32(central, static_cast<std::uint32_t>(data.size()));
        u16(central, static_cast<unsigned>(name.size())); u16(central, 0); u16(central, 0); u16(central, 0);
        u16(central, 0); u32(central, 0); u32(central, offset);
        central += name;
    }
    const auto cdOffset = static_cast<std::uint32_t>(out.size());
    out += central;
    u32(out, 0x06054b50);
    u16(out, 0); u16(out, 0);
    u16(out, static_cast<unsigned>(files.size())); u16(out, static_cast<unsigned>(files.size()));
    u32(out, static_cast<std::uint32_t>(central.size())); u32(out, cdOffset);
    u16(out, 0);
    return out;
}

} // namespace

TEST_CASE("XCI: an IP container (.xcix) is unpacked and modelled", "[frontend][xci][xcix]") {
    // chiplab axi_clock_converter_0.xcix: cc.xml and <ip>/<ip>.xci (+ output products)
    test::TempDir dir;
    const auto xcix = dir.write(
        "fifo_8X4K.xcix", storedZip({{"cc.xml", "<CoreContainer/>\n"},
                                     {"fifo_8X4K/fifo_8X4K.xci", jsonFifo("fifo_8X4K")},
                                     {"fifo_8X4K/fifo_8X4K.veo", "// instantiation template\n"}}));
    CHECK(frontend::isXciFile(xcix));
    auto m = frontend::modelFromXci(xcix);
    REQUIRE(m);
    CHECK(m.value().module == "fifo_8X4K");
    CHECK(std::ranges::any_of(m.value().notes, [](const std::string& n) { return n.find("fifo_8X4K.xcix") != std::string::npos; }));
    // in a design: the same crossings as with the .xci
    auto r = run(project(dir, fifoTop("fifo_8X4K"), {xcix}));
    REQUIRE(r->cdc);
    CHECK(count(*r, "FE-XCI-MODEL") == 1);
    CHECK(r->cdc->stats.unsynchronized == 0);
    // negative: not an archive, an archive without a .xci, an entry escaping the directory is not written
    auto bad = frontend::modelFromXci(dir.write("bad.xcix", "not a zip archive"));
    REQUIRE_FALSE(bad);
    CHECK(bad.error().message.find("not a readable ZIP archive") != std::string::npos);
    auto none = frontend::modelFromXci(dir.write("none.xcix", storedZip({{"cc.xml", "<CoreContainer/>\n"}})));
    REQUIRE_FALSE(none);
    CHECK(none.error().message.find("no .xci in the archive") != std::string::npos);
    auto slip = frontend::modelFromXci(dir.write(
        "fifo_cc.xcix", storedZip({{"../../escaped.txt", "x"}, {"fifo_cc/fifo_cc.xci", jsonFifo("fifo_cc", true)}})));
    REQUIRE(slip);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::temp_directory_path() / "escaped.txt"));
}

namespace {

// chiplab axi_2x1_mux (axi_interconnect 1.7, JSON): two slave ports, M00; a part of the ports
std::string jsonInterconnect() {
    std::string ports;
    auto add = [&](const std::string& p) { ports += (ports.empty() ? "" : ",\n        ") + p; };
    for (const char* n : {"INTERCONNECT_ACLK", "INTERCONNECT_ARESETN", "S00_AXI_ACLK", "S00_AXI_AWVALID", "S00_AXI_RREADY",
                          "S01_AXI_ACLK", "S01_AXI_ARVALID", "M00_AXI_ACLK", "M00_AXI_ARESET_IN_N", "M00_AXI_RVALID"})
        add(port(n, "in"));
    add(port("S00_AXI_AWADDR", "in", 31));
    add(port("S01_AXI_RDATA", "out", 31));
    add(port("M00_AXI_RDATA", "in", 31));
    add(port("M00_AXI_ARADDR", "out", 31));
    for (const char* n : {"S00_AXI_ARESET_OUT_N", "S00_AXI_AWREADY", "S01_AXI_ARESET_OUT_N", "M00_AXI_ARVALID",
                          "M00_AXI_ARESET_OUT_N"})
        add(port(n, "out"));
    return "{\n  \"schema\": \"xilinx.com:schema:json_instance:1.0\",\n  \"ip_inst\": {\n"
           "    \"xci_name\": \"xc\",\n"
           "    \"component_reference\": \"xilinx.com:ip:axi_interconnect:1.7\",\n"
           "    \"ip_revision\": \"30\",\n"
           "    \"parameters\": {\n      \"component_parameters\": {\n        " + param("Component_Name", "xc") +
           "\n      },\n      \"model_parameters\": {\n        " + param("C_NUM_SLAVE_PORTS", "2") + ",\n        " +
           param("C_S01_AXI_IS_ACLK_ASYNC", "1") + "\n      }\n    },\n"
           "    \"boundary\": {\n      \"ports\": {\n        " + ports + "\n      }\n    }\n  }\n}\n";
}

} // namespace

TEST_CASE("XCI: AXI Interconnect (axi_interconnect), each port in its own clock", "[frontend][xci][axi]") {
    test::TempDir dir;
    auto m = frontend::modelFromXci(dir.write("xc.xci", jsonInterconnect()), frontend::XciContext{});
    REQUIRE(m);
    const auto& v = m.value().verilog;
    INFO(v);
    for (const char* p : {"always @(posedge INTERCONNECT_ACLK[0]) oc_dom_interconnect_aclk <= ^{INTERCONNECT_ARESETN}",
                          "always @(posedge S00_AXI_ACLK[0]) oc_dom_s00_axi_aclk <= ^{S00_AXI_AWADDR, S00_AXI_AWVALID, "
                          "S00_AXI_RREADY}",
                          "always @(posedge S00_AXI_ACLK[0]) oc_S00_AXI_ARESET_OUT_N_q",
                          "always @(posedge S00_AXI_ACLK[0]) oc_S00_AXI_AWREADY_q",
                          "always @(posedge S01_AXI_ACLK[0]) oc_S01_AXI_RDATA_q",
                          "always @(posedge M00_AXI_ACLK[0]) oc_M00_AXI_ARADDR_q",
                          "always @(posedge M00_AXI_ACLK[0]) oc_dom_m00_axi_aclk <= ^{M00_AXI_ARESET_IN_N, M00_AXI_RDATA, "
                          "M00_AXI_RVALID}"})
        CHECK(v.find(p) != std::string::npos);
    CHECK(m.value().summary.find("2 slave port(s) to M00, each port in its own clock, asynchronous to the interconnect "
                                 "clock: S01") != std::string::npos);
    // negative: no port of one interface in another interface's clock
    CHECK(v.find("posedge S00_AXI_ACLK[0]) oc_S01") == std::string::npos);
    CHECK(v.find("posedge M00_AXI_ACLK[0]) oc_S0") == std::string::npos);
    // negative: without ports (an XML .xci without a component file) the IP is not modelled
    auto none = frontend::modelFromXci(dir.write("xn.xci", spirit("xn", "axi_interconnect", "1.7", {{"C_NUM_SLAVE_PORTS", "2"}})),
                                       frontend::XciContext{});
    REQUIRE_FALSE(none);
    CHECK(none.error().message.find("needs the ports of the instance") != std::string::npos);
}

namespace {

// a JSON .xci of `ip` with model parameters and ports {name, direction, left (-1: scalar)}
std::string jsonIp(const std::string& inst, const std::string& ip,
                   const std::vector<std::pair<std::string, std::string>>& params,
                   const std::vector<std::tuple<std::string, std::string, int>>& ports) {
    std::string ps, pr;
    for (const auto& [k, v] : params)
        ps += (ps.empty() ? "" : ",\n        ") + param(k, v);
    for (const auto& [n, d, l] : ports)
        pr += (pr.empty() ? "" : ",\n        ") + port(n, d.c_str(), l);
    return "{\n  \"schema\": \"xilinx.com:schema:json_instance:1.0\",\n  \"ip_inst\": {\n"
           "    \"xci_name\": \"" + inst + "\",\n"
           "    \"component_reference\": \"" + ip + "\",\n"
           "    \"parameters\": {\n      \"component_parameters\": {\n        " + param("Component_Name", inst) +
           "\n      },\n      \"model_parameters\": {\n        " + ps + "\n      }\n    },\n"
           "    \"boundary\": {\n      \"ports\": {\n        " + pr + "\n      }\n    }\n  }\n}\n";
}

frontend::XciModel modelOf(const test::TempDir& dir, const std::string& inst, const std::string& text) {
    auto m = frontend::modelFromXci(dir.write(inst + ".xci", text), frontend::XciContext{});
    if (!m)
        FAIL(m.error().message);
    INFO(m.value().verilog);
    return m.value();
}

bool has(const std::string& v, const std::string& s) {
    return v.find(s) != std::string::npos;
}

} // namespace

TEST_CASE("XCI: Processor System Reset, the reset outputs in slowest_sync_clk", "[frontend][xci][bd]") {
    test::TempDir dir;
    const auto m = modelOf(dir, "rst0",
                           jsonIp("rst0", "xilinx.com:ip:proc_sys_reset:5.0", {{"C_NUM_BUS_RST", "1"}},
                                  {{"slowest_sync_clk", "in", -1}, {"ext_reset_in", "in", -1}, {"aux_reset_in", "in", -1},
                                   {"mb_debug_sys_rst", "in", -1}, {"dcm_locked", "in", -1}, {"mb_reset", "out", -1},
                                   {"bus_struct_reset", "out", 0}, {"peripheral_reset", "out", 0},
                                   {"interconnect_aresetn", "out", 0}, {"peripheral_aresetn", "out", 0}}));
    const auto& v = m.verilog;
    INFO(v);
    for (const char* o : {"mb_reset", "bus_struct_reset", "peripheral_reset", "interconnect_aresetn", "peripheral_aresetn"})
        CHECK(has(v, std::string("always @(posedge slowest_sync_clk[0]) oc_") + o + "_q"));
    // negative: the asynchronous inputs are synchronized inside the IP, not sampled by the model
    for (const char* i : {"ext_reset_in", "aux_reset_in", "dcm_locked", "mb_debug_sys_rst"})
        CHECK_FALSE(has(v, std::string("^{") + i));
    CHECK(m.summary == "reset outputs synchronous to slowest_sync_clk");
}

TEST_CASE("XCI: Utility Vector Logic is combinational", "[frontend][xci][bd]") {
    test::TempDir dir;
    const auto a = modelOf(dir, "and0",
                           jsonIp("and0", "xilinx.com:ip:util_vector_logic:2.0", {{"C_OPERATION", "and"}, {"C_SIZE", "4"}}, {}));
    CHECK(has(a.verilog, "input  wire [3:0] Op2"));
    CHECK(has(a.verilog, "assign Res = Op1 & Op2;"));
    const auto n = modelOf(dir, "not0",
                           jsonIp("not0", "xilinx.com:ip:util_vector_logic:2.0", {{"C_OPERATION", "not"}, {"C_SIZE", "1"}}, {}));
    CHECK(has(n.verilog, "assign Res = ~Op1;"));
    // negative: NOT has no second operand, and there is no register
    CHECK_FALSE(has(n.verilog, "Op2"));
    CHECK_FALSE(has(a.verilog, "always"));
    auto bad = frontend::modelFromXci(
        dir.write("bad.xci", jsonIp("bad", "xilinx.com:ip:util_vector_logic:2.0", {{"C_OPERATION", "nand"}}, {})),
        frontend::XciContext{});
    REQUIRE_FALSE(bad);
    CHECK(has(bad.error().message, "operation 'nand' is not known"));
}

TEST_CASE("XCI: AXI protocol converter, data FIFO and APB bridge on one clock", "[frontend][xci][bd]") {
    test::TempDir dir;
    const auto pc = modelOf(dir, "pc0",
                            jsonIp("pc0", "xilinx.com:ip:axi_protocol_converter:2.1",
                                   {{"C_S_AXI_PROTOCOL", "0"}, {"C_M_AXI_PROTOCOL", "2"}},
                                   {{"aclk", "in", -1}, {"aresetn", "in", -1}, {"s_axi_awaddr", "in", 31},
                                    {"s_axi_awready", "out", -1}, {"m_axi_awaddr", "out", 31}, {"m_axi_awready", "in", -1}}));
    CHECK(has(pc.verilog, "always @(posedge aclk[0]) oc_dom_aclk <= ^{aresetn, m_axi_awready, s_axi_awaddr}"));
    CHECK(has(pc.verilog, "always @(posedge aclk[0]) oc_m_axi_awaddr_q"));
    CHECK(pc.summary == "AXI protocol converter AXI4 to AXI4-Lite, one clock (aclk)");
    const auto apb = modelOf(dir, "apb0",
                             jsonIp("apb0", "xilinx.com:ip:axi_apb_bridge:3.0", {},
                                    {{"s_axi_aclk", "in", -1}, {"s_axi_aresetn", "in", -1}, {"s_axi_wdata", "in", 31},
                                     {"m_apb_pwdata", "out", 31}, {"m_apb_prdata", "in", 31}}));
    CHECK(has(apb.verilog, "always @(posedge s_axi_aclk[0]) oc_m_apb_pwdata_q"));
    // negative: a port the model does not know is not guessed
    auto bad = frontend::modelFromXci(dir.write("df.xci", jsonIp("df", "xilinx.com:ip:axi_data_fifo:2.1", {},
                                                                 {{"aclk", "in", -1}, {"s_axi_awaddr", "in", 31},
                                                                  {"strange_clk", "in", -1}})),
                                      frontend::XciContext{});
    REQUIRE_FALSE(bad);
    CHECK(has(bad.error().message, "strange_clk"));
}

TEST_CASE("XCI: AXI data width converter and AXI4-Stream clock converter, two sides", "[frontend][xci][bd]") {
    test::TempDir dir;
    const std::vector<std::tuple<std::string, std::string, int>> ports = {
        {"s_axi_aclk", "in", -1}, {"s_axi_aresetn", "in", -1}, {"s_axi_wdata", "in", 31}, {"s_axi_wready", "out", -1},
        {"m_axi_aclk", "in", -1}, {"m_axi_aresetn", "in", -1}, {"m_axi_wdata", "out", 63}, {"m_axi_wready", "in", -1}};
    const auto async = modelOf(dir, "us0",
                               jsonIp("us0", "xilinx.com:ip:axi_dwidth_converter:2.1",
                                      {{"C_AXI_IS_ACLK_ASYNC", "1"}, {"C_S_AXI_DATA_WIDTH", "32"}, {"C_M_AXI_DATA_WIDTH", "64"}},
                                      ports));
    CHECK(has(async.verilog, "always @(posedge s_axi_aclk[0]) oc_s_axi_wready_q"));
    CHECK(has(async.verilog, "always @(posedge m_axi_aclk[0]) oc_m_axi_wdata_q"));
    CHECK(has(async.summary, "32 to 64 bits, s_axi_aclk to m_axi_aclk, asynchronous clocks"));
    // negative: synchronous without the ratio FIFO: every port in s_axi_aclk
    const auto sync = modelOf(dir, "us1",
                              jsonIp("us1", "xilinx.com:ip:axi_dwidth_converter:2.1",
                                     {{"C_AXI_IS_ACLK_ASYNC", "0"}, {"C_FIFO_MODE", "0"}}, ports));
    CHECK(has(sync.verilog, "always @(posedge s_axi_aclk[0]) oc_m_axi_wdata_q"));
    CHECK_FALSE(has(sync.verilog, "posedge m_axi_aclk"));
    const auto axis = modelOf(dir, "cc0",
                              jsonIp("cc0", "xilinx.com:ip:axis_clock_converter:1.1", {{"C_IS_ACLK_ASYNC", "1"}},
                                     {{"s_axis_aclk", "in", -1}, {"s_axis_aresetn", "in", -1}, {"s_axis_tdata", "in", 7},
                                      {"s_axis_tready", "out", -1}, {"m_axis_aclk", "in", -1}, {"m_axis_aresetn", "in", -1},
                                      {"m_axis_tdata", "out", 7}, {"m_axis_tready", "in", -1}}));
    CHECK(has(axis.verilog, "always @(posedge s_axis_aclk[0]) oc_s_axis_tready_q"));
    CHECK(has(axis.verilog, "always @(posedge m_axis_aclk[0]) oc_m_axis_tdata_q"));
    CHECK_FALSE(has(axis.verilog, "posedge s_axis_aclk[0]) oc_m_axis"));
}

TEST_CASE("XCI: AXI BRAM Controller passes s_axi_aclk to the BRAM", "[frontend][xci][bd]") {
    test::TempDir dir;
    const auto m = modelOf(dir, "bc0",
                           jsonIp("bc0", "xilinx.com:ip:axi_bram_ctrl:4.1", {},
                                  {{"s_axi_aclk", "in", -1}, {"s_axi_aresetn", "in", -1}, {"s_axi_araddr", "in", 15},
                                   {"s_axi_rdata", "out", 31}, {"bram_clk_a", "out", -1}, {"bram_rst_a", "out", -1},
                                   {"bram_addr_a", "out", 15}, {"bram_rddata_a", "in", 31}}));
    CHECK(has(m.verilog, "assign bram_clk_a = s_axi_aclk;"));
    CHECK(has(m.verilog, "always @(posedge s_axi_aclk[0]) oc_bram_addr_a_q"));
    // negative: the BRAM clock is not a register
    CHECK_FALSE(has(m.verilog, "oc_bram_clk_a_q"));
}

TEST_CASE("XCI: AXI Interrupt Controller samples only the synchronous interrupt inputs", "[frontend][xci][bd]") {
    test::TempDir dir;
    const std::vector<std::tuple<std::string, std::string, int>> ports = {
        {"s_axi_aclk", "in", -1}, {"s_axi_aresetn", "in", -1}, {"s_axi_wdata", "in", 31}, {"s_axi_rdata", "out", 31},
        {"intr", "in", 3},        {"irq", "out", -1},          {"processor_clk", "in", -1}, {"processor_rst", "in", -1},
        {"processor_ack", "in", 1}, {"interrupt_address", "out", 31}};
    // Fuxi's plic: inputs 0..2 synchronous, input 3 asynchronous
    const auto mixed = modelOf(dir, "ic0",
                               jsonIp("ic0", "xilinx.com:ip:axi_intc:4.1",
                                      {{"C_NUM_INTR_INPUTS", "4"}, {"C_ASYNC_INTR", "0xFFFFFFF8"}}, ports));
    CHECK(has(mixed.verilog, "oc_dom_s_axi_aclk <= ^{(intr & 4'h7), s_axi_aresetn, s_axi_wdata}"));
    CHECK(has(mixed.verilog, "always @(posedge s_axi_aclk[0]) oc_irq_q"));
    CHECK(has(mixed.verilog, "assign interrupt_address = 32'b0;"));
    CHECK(has(mixed.summary, "4 interrupt input(s) (1 asynchronous)"));
    // negative: all asynchronous -> intr is not sampled; synchronizers disabled -> all sampled
    const auto all = modelOf(dir, "ic1",
                             jsonIp("ic1", "xilinx.com:ip:axi_intc:4.1",
                                    {{"C_NUM_INTR_INPUTS", "4"}, {"C_ASYNC_INTR", "0xFFFFFFFF"}}, ports));
    CHECK(has(all.verilog, "oc_dom_s_axi_aclk <= ^{s_axi_aresetn"));
    CHECK_FALSE(has(all.verilog, "intr &"));
    const auto none = modelOf(dir, "ic2",
                              jsonIp("ic2", "xilinx.com:ip:axi_intc:4.1",
                                     {{"C_NUM_INTR_INPUTS", "4"}, {"C_ASYNC_INTR", "0xFFFFFFFF"},
                                      {"C_DISABLE_SYNCHRONIZERS", "1"}},
                                     ports));
    CHECK(has(none.verilog, "oc_dom_s_axi_aclk <= ^{intr, s_axi_aresetn"));
    auto fast = frontend::modelFromXci(
        dir.write("ic3.xci", jsonIp("ic3", "xilinx.com:ip:axi_intc:4.1", {{"C_HAS_FAST", "1"}}, ports)),
        frontend::XciContext{});
    REQUIRE_FALSE(fast);
    CHECK(has(fast.error().message, "fast interrupt mode"));
}

TEST_CASE("XCI: AXI Ethernet Lite, MII receive and transmit clocks", "[frontend][xci][bd]") {
    test::TempDir dir;
    const auto m = modelOf(dir, "el0",
                           jsonIp("el0", "xilinx.com:ip:axi_ethernetlite:3.0", {},
                                  {{"s_axi_aclk", "in", -1}, {"s_axi_aresetn", "in", -1}, {"s_axi_wdata", "in", 31},
                                   {"s_axi_rdata", "out", 31}, {"ip2intc_irpt", "out", -1}, {"phy_tx_clk", "in", -1},
                                   {"phy_rx_clk", "in", -1}, {"phy_crs", "in", -1}, {"phy_dv", "in", -1},
                                   {"phy_rx_data", "in", 3}, {"phy_col", "in", -1}, {"phy_rx_er", "in", -1},
                                   {"phy_rst_n", "out", -1}, {"phy_tx_en", "out", -1}, {"phy_tx_data", "out", 3},
                                   {"phy_mdio_i", "in", -1}, {"phy_mdio_o", "out", -1}, {"phy_mdio_t", "out", -1},
                                   {"phy_mdc", "out", -1}}));
    CHECK(has(m.verilog, "always @(posedge phy_rx_clk[0]) oc_dom_phy_rx_clk <= ^{phy_dv, phy_rx_data, phy_rx_er}"));
    CHECK(has(m.verilog, "always @(posedge phy_tx_clk[0]) oc_phy_tx_data_q"));
    CHECK(has(m.verilog, "always @(posedge s_axi_aclk[0]) oc_phy_mdc_q"));
    CHECK(has(m.verilog, "always @(posedge s_axi_aclk[0]) oc_ip2intc_irpt_q"));
    // negative: carrier sense, collision and MDIO input are synchronized inside the IP
    for (const char* i : {"phy_crs", "phy_col", "phy_mdio_i"}) {
        CHECK_FALSE(has(m.verilog, std::string(", ") + i));
        CHECK_FALSE(has(m.verilog, std::string("{") + i));
    }
}

TEST_CASE("XCI: System ILA observes its inputs only", "[frontend][xci][bd]") {
    test::TempDir dir;
    const auto m = modelOf(dir, "ila0",
                           jsonIp("ila0", "xilinx.com:ip:system_ila:1.1", {},
                                  {{"clk", "in", -1}, {"SLOT_0_AXI_awaddr", "in", 31}, {"resetn", "in", -1}}));
    CHECK(m.summary == "debug core, inputs only");
    CHECK_FALSE(has(m.verilog, "always"));
    // negative: an output is not a debug core's
    auto bad = frontend::modelFromXci(dir.write("ila1.xci", jsonIp("ila1", "xilinx.com:ip:system_ila:1.1", {},
                                                                   {{"clk", "in", -1}, {"trig_out", "out", -1}})),
                                      frontend::XciContext{});
    REQUIRE_FALSE(bad);
}

TEST_CASE("XCI: AXI Quad SPI with the AXI4 memory interface and the STARTUP primitive", "[frontend][xci][bd]") {
    test::TempDir dir;
    const std::vector<std::tuple<std::string, std::string, int>> ports = {
        {"ext_spi_clk", "in", -1},  {"s_axi_aclk", "in", -1},    {"s_axi_aresetn", "in", -1},
        {"s_axi4_aclk", "in", -1},  {"s_axi4_aresetn", "in", -1}, {"s_axi_wdata", "in", 31},
        {"s_axi_rdata", "out", 31}, {"s_axi4_araddr", "in", 23},  {"s_axi4_rdata", "out", 31},
        {"io0_i", "in", -1},        {"io0_o", "out", -1},         {"ss_o", "out", 0},
        {"cfgclk", "out", -1},      {"cfgmclk", "out", -1},       {"eos", "out", -1},
        {"preq", "out", -1},        {"gsr", "in", -1},            {"usrcclkts", "in", -1},
        {"ip2intc_irpt", "out", -1}};
    const auto m = modelOf(dir, "qs0",
                           jsonIp("qs0", "xilinx.com:ip:axi_quad_spi:3.2",
                                  {{"Async_Clk", "1"}, {"C_USE_STARTUP", "1"}, {"C_TYPE_OF_AXI4_INTERFACE", "1"},
                                   {"C_FAMILY", "artix7"}},
                                  ports));
    const auto& v = m.verilog;
    INFO(v);
    CHECK(has(v, "always @(posedge s_axi4_aclk) oc_axi4_mix <= ^{s_axi4_araddr, s_axi4_aresetn}"));
    CHECK(has(v, "always @(posedge s_axi4_aclk) oc_s_axi4_rdata <= {32{oc_to_axi4 ^ oc_axi4_mix}}"));
    CHECK(has(v, "u_oc_tx4_fifo (.wr_clk(s_axi4_aclk), .rd_clk(ext_spi_clk)"));
    CHECK(has(v, "STARTUPE2 STARTUP2_7SERIES_inst (.CFGCLK(cfgclk), .CFGMCLK(cfgmclk), .EOS(eos), .PREQ(preq)"));
    CHECK(has(v, ".USRCCLKO(oc_sck)"));
    CHECK(has(m.summary, "AXI4 memory interface on s_axi4_aclk"));
    // negative: the AXI4 side is not in the AXI4-Lite clock; the STARTUP inputs are not sampled
    CHECK_FALSE(has(v, "posedge s_axi_aclk) oc_s_axi4"));
    CHECK_FALSE(has(v, "gsr}"));
    CHECK_FALSE(has(v, ", gsr"));
    // UltraScale: STARTUPE3
    const auto us = modelOf(dir, "qs1",
                            jsonIp("qs1", "xilinx.com:ip:axi_quad_spi:3.2",
                                   {{"Async_Clk", "1"}, {"C_USE_STARTUP", "1"}, {"C_FAMILY", "kintexu"}}, ports));
    CHECK(has(us.verilog, "STARTUPE3 STARTUP3_ULTRASCALE_inst"));
}

TEST_CASE("XCI: the cell interface of a component file and of a JSON .xci", "[frontend][xci][bd]") {
    const std::string xml =
        "<spirit:component xmlns:spirit=\"x\">\n<spirit:busInterfaces>\n<spirit:busInterface>\n"
        "<spirit:name>S01_AXI</spirit:name>\n<spirit:portMaps>\n"
        "<spirit:portMap><spirit:logicalPort><spirit:name>AWADDR</spirit:name></spirit:logicalPort>"
        "<spirit:physicalPort><spirit:name>s_axi_awaddr</spirit:name><spirit:vector>"
        "<spirit:left spirit:format=\"long\" spirit:dependency=\"(2 * x) - 1 > 0\">63</spirit:left>"
        "<spirit:right spirit:format=\"long\">32</spirit:right></spirit:vector></spirit:physicalPort></spirit:portMap>\n"
        "<spirit:portMap><spirit:logicalPort><spirit:name>AWVALID</spirit:name></spirit:logicalPort>"
        "<spirit:physicalPort><spirit:name>s_axi_awvalid</spirit:name></spirit:physicalPort></spirit:portMap>\n"
        "</spirit:portMaps>\n</spirit:busInterface>\n</spirit:busInterfaces>\n<spirit:model><spirit:ports>\n"
        "<spirit:port><spirit:name>s_axi_awaddr</spirit:name><spirit:wire><spirit:direction>in</spirit:direction>"
        "<spirit:vector><spirit:left spirit:format=\"long\" spirit:dependency=\"a > b\">63</spirit:left>"
        "<spirit:right spirit:format=\"long\">0</spirit:right></spirit:vector>"
        "<spirit:driver><spirit:defaultValue spirit:format=\"long\">0</spirit:defaultValue></spirit:driver>"
        "</spirit:wire></spirit:port>\n"
        "<spirit:port><spirit:name>s_axi_awvalid</spirit:name><spirit:wire><spirit:direction>in</spirit:direction>"
        "<spirit:driver><spirit:defaultValue spirit:format=\"long\">1</spirit:defaultValue></spirit:driver>"
        "</spirit:wire></spirit:port>\n"
        "</spirit:ports>\n<spirit:modelParameters>\n"
        "<spirit:modelParameter spirit:dataType=\"integer\"><spirit:name>BUS_WIDTH</spirit:name>"
        "<spirit:value spirit:format=\"long\" spirit:resolve=\"generated\">6</spirit:value></spirit:modelParameter>\n"
        "<spirit:modelParameter><spirit:name>SIMULATION</spirit:name><spirit:value spirit:format=\"bitString\" "
        "spirit:bitStringLength=\"1\">&quot;0&quot;</spirit:value></spirit:modelParameter>\n"
        "<spirit:modelParameter><spirit:name>MODE</spirit:name><spirit:value spirit:format=\"string\">fast"
        "</spirit:value></spirit:modelParameter>\n"
        "</spirit:modelParameters>\n</spirit:model>\n</spirit:component>\n";
    const auto ci = frontend::cellInterfaceFromText(xml);
    REQUIRE(ci);
    REQUIRE(ci->ports.size() == 2);
    CHECK(ci->ports[0].range == std::pair{63LL, 0LL}); // the dependency with '>' does not hide the width
    REQUIRE(ci->interfaces.size() == 1);
    const auto& bi = ci->interfaces[0];
    CHECK(bi.name == "S01_AXI");
    REQUIRE(bi.ports.size() == 2);
    CHECK(bi.ports[0].logical == "AWADDR");
    CHECK(bi.ports[0].slice == std::pair{63LL, 32LL});
    CHECK_FALSE(bi.ports[1].slice);
    CHECK(ci->defaults.at("s_axi_awvalid") == 1);
    REQUIRE(ci->parameters.size() == 3);
    CHECK(ci->parameters[0] == std::pair<std::string, std::string>{"BUS_WIDTH", "6"});
    CHECK(ci->parameters[1] == std::pair<std::string, std::string>{"SIMULATION", "1'b0"});
    CHECK(ci->parameters[2] == std::pair<std::string, std::string>{"MODE", "\"fast\""});
    // JSON .xci: ports and port maps of the boundary
    const std::string json =
        "{ \"ip_inst\": { \"xci_name\": \"x\", \"component_reference\": \"xilinx.com:ip:axi_crossbar:2.1\",\n"
        "  \"boundary\": { \"ports\": { \"s_axi_awid\": [ { \"direction\": \"in\", \"size_left\": \"7\", "
        "\"size_right\": \"0\", \"driver_value\": \"0\" } ] },\n"
        "  \"interfaces\": { \"S01_AXI\": { \"vlnv\": \"xilinx.com:interface:aximm:1.0\", \"port_maps\": {\n"
        "    \"AWID\": [ { \"physical_name\": \"s_axi_awid\", \"physical_left\": \"7\", \"physical_right\": \"4\" } ] } } } } } }";
    const auto cj = frontend::cellInterfaceFromText(json);
    REQUIRE(cj);
    REQUIRE(cj->interfaces.size() == 1);
    CHECK(cj->interfaces[0].ports[0].slice == std::pair{7LL, 4LL});
    CHECK(cj->defaults.at("s_axi_awid") == 0);
    // negative: no ports
    CHECK_FALSE(frontend::cellInterfaceFromText("<spirit:component></spirit:component>"));
    CHECK_FALSE(frontend::cellInterfaceFromText("{ \"ip_inst\": { } }"));
}

namespace {

std::vector<std::tuple<std::string, std::string, int>> ps7Ports() {
    return {{"FCLK_CLK0", "out", -1},         {"FCLK_CLK1", "out", -1},        {"FCLK_RESET0_N", "out", -1},
            {"M_AXI_GP0_ACLK", "in", -1},     {"M_AXI_GP0_ARVALID", "out", -1}, {"M_AXI_GP0_ARADDR", "out", 31},
            {"M_AXI_GP0_ARREADY", "in", -1},  {"S_AXI_HP0_ACLK", "in", -1},    {"S_AXI_HP0_RDATA", "out", 63},
            {"S_AXI_HP0_ARVALID", "in", -1},  {"IRQ_F2P", "in", 1},            {"GPIO_O", "out", 3},
            {"GPIO_I", "in", 3},              {"ENET0_GMII_TX_CLK", "in", -1}, {"ENET0_GMII_TXD", "out", 7},
            {"MIO", "inout", 53},             {"DDR_DQ", "inout", 31},         {"PS_CLK", "inout", -1}};
}

} // namespace

TEST_CASE("XCI: Zynq-7000 Processing System, fabric clocks and AXI ports", "[frontend][xci][bd][ps7]") {
    test::TempDir dir;
    const auto m = modelOf(dir, "ps7",
                           jsonIp("ps7", "xilinx.com:ip:processing_system7:5.5",
                                  {{"PCW_ACT_FPGA0_PERIPHERAL_FREQMHZ", "100.000000"},
                                   {"PCW_ACT_FPGA1_PERIPHERAL_FREQMHZ", "50.000000"}},
                                  ps7Ports()));
    const auto& v = m.verilog;
    INFO(v);
    CHECK(has(v, "OPENCDC_CLOCK_PIN = \"FCLK_CLK0\", OPENCDC_CLOCK_ALIAS = \"clk_fpga_0\", OPENCDC_CLOCK_PERIOD = 10"));
    CHECK(has(v, "OPENCDC_CLOCK_PIN = \"FCLK_CLK1\", OPENCDC_CLOCK_ALIAS = \"clk_fpga_1\", OPENCDC_CLOCK_PERIOD = 20"));
    CHECK(has(v, "inout  wire [53:0] MIO"));
    CHECK(has(v, "always @(posedge M_AXI_GP0_ACLK[0]) oc_M_AXI_GP0_ARADDR_q"));
    CHECK(has(v, "oc_dom_m_axi_gp0_aclk <= ^{M_AXI_GP0_ARREADY}"));
    CHECK(has(v, "always @(posedge S_AXI_HP0_ACLK[0]) oc_S_AXI_HP0_RDATA_q"));
    CHECK(has(v, "always @(posedge ENET0_GMII_TX_CLK[0]) oc_ENET0_GMII_TXD_q"));
    CHECK(has(v, "OPENCDC_CLOCK_PIN = \"cpu_1x\""));
    CHECK(has(v, "oc_GPIO_O_q"));
    CHECK(has(v, "opencdc_async_source u_oc_FCLK_RESET0_N"));
    CHECK(has(m.summary, "FCLK_CLK0 100.0 MHz (clk_fpga_0), FCLK_CLK1 50.0 MHz (clk_fpga_1)"));
    // negative: interrupts, EMIO inputs and the device pins are not read
    for (const char* i : {"IRQ_F2P", "GPIO_I", "MIO", "DDR_DQ"}) {
        CHECK_FALSE(has(v, std::string("{") + i));
        CHECK_FALSE(has(v, std::string(", ") + i + "}"));
    }
    CHECK_FALSE(has(v, "assign MIO"));
}

TEST_CASE("XCI: the Zynq fabric clocks in a design, named as in Vivado", "[frontend][xci][bd][ps7]") {
    test::TempDir dir;
    auto cfg = project(dir,
                       "module top(input [3:0] gi, output reg qb, output reg qg);\n"
                       "  wire c0, c1, rn; wire [3:0] go;\n"
                       "  ps7 u (.FCLK_CLK0(c0), .FCLK_CLK1(c1), .FCLK_RESET0_N(rn), .M_AXI_GP0_ACLK(c0),\n"
                       "         .M_AXI_GP0_ARREADY(1'b1), .S_AXI_HP0_ACLK(c1), .S_AXI_HP0_ARVALID(1'b0),\n"
                       "         .IRQ_F2P({qa, qb}), .GPIO_O(go), .GPIO_I(gi), .ENET0_GMII_TX_CLK(1'b0));\n"
                       "  reg qa; always @(posedge c0) qa <= ~qa;\n"
                       "  always @(posedge c1) qb <= qa;\n"
                       "  always @(posedge c0) qg <= go[0];\n"
                       "endmodule\n",
                       {dir.write("ps7.xci", jsonIp("ps7", "xilinx.com:ip:processing_system7:5.5",
                                                    {{"PCW_ACT_FPGA0_PERIPHERAL_FREQMHZ", "100.000000"},
                                                     {"PCW_ACT_FPGA1_PERIPHERAL_FREQMHZ", "50.000000"}},
                                                    ps7Ports()))});
    // Vivado's clock names in the user's constraints
    cfg.constraints = {dir.write("z.xdc", "set_clock_groups -asynchronous -group [get_clocks clk_fpga_0] "
                                          "-group [get_clocks clk_fpga_1]\n")};
    const auto r = run(cfg);
    REQUIRE(r->clocks);
    bool c0 = false;
    for (const auto& c : r->clocks->clocks)
        c0 = c0 || (std::ranges::find(c.aliases, "clk_fpga_0") != c.aliases.end() && c.periodNs &&
                    *c.periodNs == 10.0);
    CHECK(c0);
    CHECK(find(*r, "CON-UNMATCHED") == nullptr);
    REQUIRE(r->cdc);
    // qa (FCLK_CLK0) -> qb (FCLK_CLK1): asynchronous by the constraint, unsynchronized
    CHECK(r->cdc->stats.unsynchronized >= 1);
    // the EMIO GPIO output is in the PS clock CPU_1x: GPIO_O -> qg (FCLK_CLK0) is a crossing
    CHECK(std::ranges::any_of(r->cdc->paths, [](const auto& p) { return p.dstName.find("qg") != std::string::npos; }));
}

TEST_CASE("XCI: SmartConnect, each interface in the clock associated with it", "[frontend][xci][bd]") {
    test::TempDir dir;
    auto sc = [](bool assoc) {
        const std::string clk1 = assoc ? R"(, "parameters": { "ASSOCIATED_BUSIF": [ { "value": "M00_AXI" } ] })" : "";
        const std::string clk0 = assoc ? R"(, "parameters": { "ASSOCIATED_BUSIF": [ { "value": "S00_AXI:S01_AXI" } ] })" : "";
        return std::string(R"({ "ip_inst": { "xci_name": "sc0", "component_reference": "xilinx.com:ip:smartconnect:1.0",
          "boundary": {
            "ports": { "aclk": [ { "direction": "in" } ], "aclk1": [ { "direction": "in" } ],
                       "aresetn": [ { "direction": "in" } ],
                       "S00_AXI_awaddr": [ { "direction": "in", "size_left": "31", "size_right": "0" } ],
                       "S00_AXI_awready": [ { "direction": "out" } ],
                       "M00_AXI_awaddr": [ { "direction": "out", "size_left": "31", "size_right": "0" } ],
                       "M00_AXI_awready": [ { "direction": "in" } ] },
            "interfaces": {
              "CLK.aclk": { "vlnv": "xilinx.com:signal:clock:1.0", "port_maps": { "CLK": [ { "physical_name": "aclk" } ] })") +
               clk0 + R"( },
              "CLK.aclk1": { "vlnv": "xilinx.com:signal:clock:1.0", "port_maps": { "CLK": [ { "physical_name": "aclk1" } ] })" +
               clk1 + R"( } } } } })";
    };
    const auto m = modelOf(dir, "sc0", sc(true));
    CHECK(has(m.verilog, "always @(posedge aclk[0]) oc_S00_AXI_awready_q"));
    CHECK(has(m.verilog, "always @(posedge aclk1[0]) oc_M00_AXI_awaddr_q"));
    CHECK(has(m.verilog, "oc_dom_aclk <= ^{S00_AXI_awaddr, aresetn}"));
    CHECK(m.summary == "2 clock(s): aclk: S00_AXI, S01_AXI; aclk1: M00_AXI");
    // negative: the master side is not in aclk; without the associations the IP is not modelled
    CHECK_FALSE(has(m.verilog, "posedge aclk[0]) oc_M00"));
    auto none = frontend::modelFromXci(dir.write("sc1.xci", sc(false)), frontend::XciContext{});
    REQUIRE_FALSE(none);
    CHECK(has(none.error().message, "ASSOCIATED_BUSIF"));
}

TEST_CASE("XCI: the component file in <project>.gen (Vivado 2020.2+)", "[frontend][xci][bd]") {
    CHECK(frontend::generatedProductsDir("/w/p.srcs/sources_1/bd/d/ip/x") ==
          std::filesystem::path("/w/p.gen/sources_1/bd/d/ip/x"));
    CHECK_FALSE(frontend::generatedProductsDir("/w/src/ip/x"));
    test::TempDir dir;
    // an XML .xci in .srcs, its component file (with the clock associations) in .gen
    const auto xci = dir.write("p.srcs/sources_1/ip/sc2/sc2.xci", spirit("sc2", "smartconnect", "1.0", {}));
    dir.write("p.gen/sources_1/ip/sc2/sc2.xml",
              "<spirit:component xmlns:spirit=\"x\">\n<spirit:busInterfaces>\n"
              "<spirit:busInterface><spirit:name>CLK.aclk</spirit:name>"
              "<spirit:busType spirit:vendor=\"xilinx.com\" spirit:library=\"signal\" spirit:name=\"clock\" "
              "spirit:version=\"1.0\"/><spirit:portMaps><spirit:portMap><spirit:logicalPort><spirit:name>CLK"
              "</spirit:name></spirit:logicalPort><spirit:physicalPort><spirit:name>aclk</spirit:name>"
              "</spirit:physicalPort></spirit:portMap></spirit:portMaps><spirit:parameters><spirit:parameter>"
              "<spirit:name>ASSOCIATED_BUSIF</spirit:name><spirit:value spirit:id=\"a\">S00_AXI:M00_AXI</spirit:value>"
              "</spirit:parameter></spirit:parameters></spirit:busInterface>\n</spirit:busInterfaces>\n"
              "<spirit:model><spirit:ports>\n"
              "<spirit:port><spirit:name>aclk</spirit:name><spirit:wire><spirit:direction>in</spirit:direction>"
              "</spirit:wire></spirit:port>\n"
              "<spirit:port><spirit:name>S00_AXI_awvalid</spirit:name><spirit:wire><spirit:direction>in"
              "</spirit:direction></spirit:wire></spirit:port>\n"
              "<spirit:port><spirit:name>M00_AXI_awvalid</spirit:name><spirit:wire><spirit:direction>out"
              "</spirit:direction></spirit:wire></spirit:port>\n"
              "</spirit:ports></spirit:model>\n</spirit:component>\n");
    auto m = frontend::modelFromXci(xci, frontend::XciContext{});
    REQUIRE(m);
    CHECK(has(m.value().verilog, "always @(posedge aclk[0]) oc_M00_AXI_awvalid_q"));
    CHECK(m.value().summary == "1 clock(s): aclk: M00_AXI, S00_AXI");
}

namespace {

// the component file of an IP instance with its output products: synthesis view (ipshared HDL), wrapper view
std::string instanceComponent(const std::string& inst, bool views) {
    std::string s = "<spirit:component xmlns:spirit=\"x\">\n<spirit:model>\n";
    if (views)
        s += "<spirit:views>\n"
             "<spirit:view><spirit:name>xilinx_anylanguagesynthesis</spirit:name><spirit:envIdentifier>"
             ":vivado.xilinx.com:synthesis</spirit:envIdentifier><spirit:modelName>core</spirit:modelName>"
             "<spirit:fileSetRef><spirit:localName>synth_fs</spirit:localName></spirit:fileSetRef></spirit:view>\n"
             "<spirit:view><spirit:name>xilinx_anylanguagesynthesiswrapper</spirit:name><spirit:envIdentifier>"
             ":vivado.xilinx.com:synthesis.wrapper</spirit:envIdentifier><spirit:modelName>" + inst +
             "</spirit:modelName><spirit:fileSetRef><spirit:localName>wrap_fs</spirit:localName></spirit:fileSetRef>"
             "</spirit:view>\n</spirit:views>\n";
    s += "<spirit:ports>\n";
    for (const auto& [n, d] : std::vector<std::pair<std::string, std::string>>{
             {"clk_a", "in"}, {"clk_b", "in"}, {"d", "in"}, {"q", "out"}})
        s += "<spirit:port><spirit:name>" + n + "</spirit:name><spirit:wire><spirit:direction>" + d +
             "</spirit:direction></spirit:wire></spirit:port>\n";
    s += "</spirit:ports>\n</spirit:model>\n<spirit:fileSets>\n"
         "<spirit:fileSet><spirit:name>synth_fs</spirit:name><spirit:file><spirit:name>../../ipshared/ab12/hdl/core.v"
         "</spirit:name><spirit:fileType>verilogSource</spirit:fileType></spirit:file></spirit:fileSet>\n"
         "<spirit:fileSet><spirit:name>wrap_fs</spirit:name><spirit:file><spirit:name>synth/" + inst +
         ".v</spirit:name><spirit:fileType>verilogSource</spirit:fileType></spirit:file></spirit:fileSet>\n"
         "</spirit:fileSets>\n</spirit:component>\n";
    return s;
}

const char* kIpTop = "module top(input clk_a, input clk_b, input d, output q);\n"
                     "  gen_ip u (.clk_a(clk_a), .clk_b(clk_b), .d(d), .q(q));\n"
                     "endmodule\n";

} // namespace

TEST_CASE("XCI: an IP without a model from its generated HDL", "[frontend][xci][generated]") {
    test::TempDir dir;
    const auto xci = dir.write("bd/ip/gen_ip/gen_ip.xci", spirit("gen_ip", "my_core", "1.0", {}));
    dir.write("bd/ip/gen_ip/gen_ip.xml", instanceComponent("gen_ip", true));
    // the IP's own HDL: a one-flop crossing from clk_a to clk_b
    dir.write("bd/ipshared/ab12/hdl/core.v", "module core(input clk_a, input clk_b, input d, output reg q);\n"
                                             "  reg a; always @(posedge clk_a) a <= d;\n"
                                             "  always @(posedge clk_b) q <= a;\nendmodule\n");
    dir.write("bd/ip/gen_ip/synth/gen_ip.v", "module gen_ip(input clk_a, input clk_b, input d, output q);\n"
                                             "  core c (.clk_a(clk_a), .clk_b(clk_b), .d(d), .q(q));\nendmodule\n");
    auto m = frontend::modelFromXci(xci, frontend::XciContext{});
    REQUIRE(m);
    CHECK(m.value().verilog.empty());
    REQUIRE(m.value().extraSources.size() == 2);
    CHECK(m.value().extraSources[0].filename() == "core.v");
    CHECK(m.value().extraSources[1].filename() == "gen_ip.v");
    CHECK(m.value().summary == "its generated HDL (synthesis view, 2 file(s))");
    // in a design: the IP's own crossing is found
    const auto r = run(project(dir, kIpTop, {xci}));
    const auto* d = find(*r, "FE-XCI-MODEL");
    REQUIRE(d);
    CHECK(d->message.find("analyzed from its output products") != std::string::npos);
    REQUIRE(r->cdc);
    CHECK(r->cdc->stats.unsynchronized >= 1);
    CHECK(std::ranges::any_of(r->cdc->paths, [](const auto& p) { return p.dstName.find("u.c.q") != std::string::npos; }));
}

TEST_CASE("XCI: an IP without a model from its simulation netlist; encrypted files are not used",
          "[frontend][xci][generated]") {
    test::TempDir dir;
    const auto xci = dir.write("bd/ip/gen_ip/gen_ip.xci", spirit("gen_ip", "my_core", "1.0", {}));
    dir.write("bd/ip/gen_ip/gen_ip.xml", instanceComponent("gen_ip", true));
    // the HDL is encrypted: the netlist is used (FDRE synchronizer, CARRY4, RAM32M as Vivado writes them)
    dir.write("bd/ipshared/ab12/hdl/core.v", "`pragma protect begin_protected\n`pragma protect end_protected\n");
    dir.write("bd/ip/gen_ip/synth/gen_ip.v", "module gen_ip(input clk_a, input clk_b, input d, output q);\nendmodule\n");
    dir.write("bd/ip/gen_ip/gen_ip_sim_netlist.v",
              "`timescale 1 ps / 1 ps\n"
              "(* CHECK_LICENSE_TYPE = \"gen_ip,core,{}\" *)\n"
              "module gen_ip (clk_a, clk_b, d, q);\n  input clk_a; input clk_b; input d; output q;\n"
              "  wire a, s1, s2; wire [3:0] co, o; wire [1:0] doa;\n"
              "  FDRE #(.INIT(1'b0)) a_reg (.C(clk_a), .CE(1'b1), .D(d), .Q(a), .R(1'b0));\n"
              "  (* ASYNC_REG *) FDRE #(.INIT(1'b0)) s1_reg (.C(clk_b), .CE(1'b1), .D(a), .Q(s1), .R(1'b0));\n"
              "  (* ASYNC_REG *) FDRE #(.INIT(1'b0)) s2_reg (.C(clk_b), .CE(1'b1), .D(s1), .Q(s2), .R(1'b0));\n"
              "  CARRY4 cy (.CI(1'b0), .CYINIT(s2), .DI(4'b0), .S({3'b0, s2}), .CO(co), .O(o));\n"
              "  RAM32M ram (.ADDRA(5'd0), .ADDRB(5'd0), .ADDRC(5'd0), .ADDRD(5'd0), .DIA({1'b0, a}), .DIB(2'b0),\n"
              "              .DIC(2'b0), .DID(2'b0), .DOA(doa), .WCLK(clk_a), .WE(1'b1));\n"
              "  FDRE #(.INIT(1'b0)) q_reg (.C(clk_b), .CE(1'b1), .D(o[0] ^ doa[0]), .Q(q), .R(1'b0));\n"
              "endmodule\n"
              "`ifndef GLBL\n`define GLBL\nmodule glbl ();\n  wire GSR;\nendmodule\n`endif\n");
    auto m = frontend::modelFromXci(xci, frontend::XciContext{});
    REQUIRE(m);
    REQUIRE(m.value().extraSources.size() == 1);
    CHECK(m.value().extraSources[0].filename() == "gen_ip_sim_netlist.v");
    CHECK(m.value().summary == "its simulation netlist");
    CHECK(has(m.value().notes.at(0), "encrypted"));
    const auto r = run(project(dir, kIpTop, {xci}));
    CHECK(r->count(Severity::Error) == 0);
    REQUIRE(r->cdc);
    // a -> s1 -> s2 is a two-flop synchronizer; the RAM read by q_reg (clk_b) is an unsynchronized crossing
    CHECK(r->cdc->stats.synchronized >= 1);
    CHECK(r->cdc->stats.unsynchronized >= 1);
    // negative: an encrypted netlist too -> not modelled, with the reason
    dir.write("bd/ip/gen_ip/gen_ip_sim_netlist.v", "`protect begin_protected\n`protect end_protected\n");
    auto none = frontend::modelFromXci(xci, frontend::XciContext{});
    REQUIRE_FALSE(none);
    CHECK(has(none.error().notes.at(0), "simulation netlist encrypted"));
    // negative: no output products at all
    test::TempDir dir2;
    const auto x2 = dir2.write("gen_ip.xci", spirit("gen_ip", "my_core", "1.0", {}));
    auto bare = frontend::modelFromXci(x2, frontend::XciContext{});
    REQUIRE_FALSE(bare);
    CHECK(has(bare.error().notes.at(0), "no component file"));
}
