// Vivado block designs (.bd): the module generated from the cells, nets and
// interface nets, and its analysis with the IP models.

#include "core/Diagnostic.h"
#include "core/Progress.h"
#include "engine/AnalysisSession.h"
#include "engine/ProjectConfig.h"
#include "frontend/BlockDesign.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace opencdc;

namespace {

bool has(const std::string& v, const std::string& s) {
    return v.find(s) != std::string::npos;
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

const Diagnostic* find(const engine::AnalysisResult& r, std::string_view code) {
    auto it = std::ranges::find(r.diagnostics, code, &Diagnostic::code);
    return it == r.diagnostics.end() ? nullptr : &*it;
}

// the AXI4-Stream clock converter as Vivado 2020.2+ writes its .xci (JSON, with the boundary)
const char* kConverterXci = R"({
  "schema": "xilinx.com:schema:json_instance:1.0",
  "ip_inst": {
    "xci_name": "design_1_cc_0",
    "component_reference": "xilinx.com:ip:axis_clock_converter:1.1",
    "parameters": { "model_parameters": { "C_IS_ACLK_ASYNC": [ { "value": "1" } ] } },
    "boundary": {
      "ports": {
        "s_axis_aclk": [ { "direction": "in" } ],
        "s_axis_aresetn": [ { "direction": "in", "driver_value": "1" } ],
        "s_axis_tdata": [ { "direction": "in", "size_left": "7", "size_right": "0" } ],
        "s_axis_tvalid": [ { "direction": "in" } ],
        "s_axis_tready": [ { "direction": "out" } ],
        "m_axis_aclk": [ { "direction": "in" } ],
        "m_axis_aresetn": [ { "direction": "in", "driver_value": "1" } ],
        "m_axis_tdata": [ { "direction": "out", "size_left": "7", "size_right": "0" } ],
        "m_axis_tvalid": [ { "direction": "out" } ],
        "m_axis_tready": [ { "direction": "in" } ]
      },
      "interfaces": {
        "S_AXIS": { "vlnv": "xilinx.com:interface:axis:1.0", "mode": "slave", "port_maps": {
          "TDATA": [ { "physical_name": "s_axis_tdata" } ], "TVALID": [ { "physical_name": "s_axis_tvalid" } ],
          "TREADY": [ { "physical_name": "s_axis_tready" } ] } },
        "M_AXIS": { "vlnv": "xilinx.com:interface:axis:1.0", "mode": "master", "port_maps": {
          "TDATA": [ { "physical_name": "m_axis_tdata" } ], "TVALID": [ { "physical_name": "m_axis_tvalid" } ],
          "TREADY": [ { "physical_name": "m_axis_tready" } ] } }
      }
    }
  }
})";

const char* kRtl = R"(
module producer (input clk, output [7:0] m_tdata, output m_tvalid, input m_tready);
  reg [7:0] cnt = 8'd0;
  always @(posedge clk) if (m_tready) cnt <= cnt + 8'd1;
  assign m_tdata = cnt;
  assign m_tvalid = 1'b1;
endmodule
module consumer (input clk, input [7:0] s_tdata, input s_tvalid, output s_tready, output [7:0] q,
                 output [3:0] stat_o);
  reg [7:0] r = 8'd0;
  always @(posedge clk) if (s_tvalid) r <= s_tdata;
  assign s_tready = 1'b1;
  assign q = r;
  assign stat_o = r[3:0];
endmodule
)";

/// design_1: producer (clk_a) -> [cc: axis_clock_converter, when `converter`] -> grp/cons (clk_b) -> led
std::string designBd(bool converter) {
    const std::string producer = R"(
      "prod": {
        "vlnv": "xilinx.com:module_ref:producer:1.0", "xci_name": "design_1_prod_0",
        "reference_info": { "ref_type": "hdl", "ref_name": "producer" },
        "ports": { "clk": { "type": "clk", "direction": "I" } },
        "interface_ports": { "M_AXIS": { "mode": "Master", "vlnv": "xilinx.com:interface:axis_rtl:1.0", "port_maps": {
          "TDATA": { "physical_name": "m_tdata", "direction": "O", "left": "7", "right": "0" },
          "TVALID": { "physical_name": "m_tvalid", "direction": "O" },
          "TREADY": { "physical_name": "m_tready", "direction": "I" } } } }
      },)";
    const std::string cc = converter ? R"(
      "cc": { "vlnv": "xilinx.com:ip:axis_clock_converter:1.1", "xci_name": "design_1_cc_0" },)"
                                     : "";
    const std::string grp = R"(
      "grp": {
        "interface_ports": { "S_AXIS": { "mode": "Slave", "vlnv": "xilinx.com:interface:axis_rtl:1.0" },
                             "STAT": { "mode": "Master", "vlnv": "xilinx.com:interface:gpio_rtl:1.0" } },
        "ports": { "CLK": { "type": "clk", "direction": "I" }, "Q": { "direction": "O", "left": "7", "right": "0" } },
        "components": {
          "cons": {
            "vlnv": "xilinx.com:module_ref:consumer:1.0", "xci_name": "design_1_cons_0",
            "reference_info": { "ref_type": "hdl", "ref_name": "consumer" },
            "ports": { "clk": { "type": "clk", "direction": "I" },
                       "q": { "direction": "O", "left": "7", "right": "0" } },
            "interface_ports": {
              "S_AXIS": { "mode": "Slave", "vlnv": "xilinx.com:interface:axis_rtl:1.0", "port_maps": {
                "TDATA": { "physical_name": "s_tdata", "direction": "I", "left": "7", "right": "0" },
                "TVALID": { "physical_name": "s_tvalid", "direction": "I" },
                "TREADY": { "physical_name": "s_tready", "direction": "O" } } },
              "STAT": { "mode": "Master", "vlnv": "xilinx.com:interface:gpio_rtl:1.0", "port_maps": {
                "TRI_O": { "physical_name": "stat_o", "direction": "O", "left": "3", "right": "0" } } } }
          }
        },
        "interface_nets": {
          "S_AXIS_1": { "interface_ports": [ "S_AXIS", "cons/S_AXIS" ] },
          "cons_STAT": { "interface_ports": [ "STAT", "cons/STAT" ] }
        },
        "nets": {
          "CLK_1": { "ports": [ "CLK", "cons/clk" ] },
          "cons_q": { "ports": [ "cons/q", "Q" ] }
        }
      }
    },)";
    const std::string inets = converter ? R"(
      "prod_M_AXIS": { "interface_ports": [ "prod/M_AXIS", "cc/S_AXIS" ] },
      "cc_M_AXIS": { "interface_ports": [ "cc/M_AXIS", "grp/S_AXIS" ] },)"
                                        : R"(
      "prod_M_AXIS": { "interface_ports": [ "prod/M_AXIS", "grp/S_AXIS" ] },)";
    const std::string nets = converter ? R"(
      "clk_a_1": { "ports": [ "clk_a", "prod/clk", "cc/s_axis_aclk" ] },
      "clk_b_1": { "ports": [ "clk_b", "cc/m_axis_aclk", "grp/CLK" ] },)"
                                       : R"(
      "clk_a_1": { "ports": [ "clk_a", "prod/clk" ] },
      "clk_b_1": { "ports": [ "clk_b", "grp/CLK" ] },)";
    return R"({
  "design": {
    "design_info": { "name": "design_1", "tool_version": "2022.2" },
    "ports": {
      "clk_a": { "type": "clk", "direction": "I" },
      "clk_b": { "type": "clk", "direction": "I" },
      "led": { "direction": "O", "left": "7", "right": "0" }
    },
    "interface_ports": { "STAT": { "mode": "Master", "vlnv": "xilinx.com:interface:gpio_rtl:1.0" } },
    "components": {)" +
           producer + cc + grp + R"(
    "interface_nets": {)" +
           inets + R"(
      "grp_STAT": { "interface_ports": [ "STAT", "grp/STAT" ] }
    },
    "nets": {)" +
           nets + R"(
      "grp_Q": { "ports": [ "grp/Q", "led" ] }
    }
  }
})";
}

engine::ProjectConfig project(const test::TempDir& dir, bool converter) {
    const auto bd = dir.write(converter ? "bd/design_1/design_1.bd" : "bd2/design_1/design_1.bd", designBd(converter));
    if (converter)
        dir.write("bd/design_1/ip/design_1_cc_0/design_1_cc_0.xci", kConverterXci);
    engine::ProjectConfig cfg;
    cfg.name = "bd";
    cfg.top = "design_1";
    cfg.sources = {dir.write("rtl.v", kRtl), bd};
    cfg.sourceLibraries.assign(cfg.sources.size(), "");
    cfg.constraints = {dir.write("c.sdc", "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                                              "create_clock -name clk_b -period 8 [get_ports clk_b]\n"
                                              "set_clock_groups -asynchronous -group clk_a -group clk_b\n")};
    return cfg;
}

} // namespace

TEST_CASE("Block design: the module of a .bd, interface nets expanded through the port maps", "[frontend][bd]") {
    test::TempDir dir;
    (void)project(dir, true);
    auto r = frontend::blockDesignFromFile(dir.path() / "bd/design_1/design_1.bd");
    REQUIRE(r);
    const auto& b = r.value();
    const auto& v = b.verilog;
    INFO(v);
    CHECK(b.name == "design_1");
    CHECK(b.cells == 3);
    CHECK(b.hierarchies == 1);
    REQUIRE(b.ipFiles.size() == 1);
    CHECK(b.ipFiles[0].filename() == "design_1_cc_0.xci");
    for (const char* s : {"module design_1 (", "input  wire clk_a,", "output wire [7:0] led,",
                          // the design's interface port: one port per signal
                          "output wire [3:0] STAT_tri_o",
                          // the interface nets carry the names Vivado gives them
                          "wire [7:0] prod_M_AXIS_TDATA;", "wire [0:0] cc_M_AXIS_TREADY;",
                          "producer prod (", ".m_tdata(prod_M_AXIS_TDATA)", ".m_tready(prod_M_AXIS_TREADY)",
                          "design_1_cc_0 cc (", ".s_axis_aclk(clk_a)", ".m_axis_aclk(clk_b)",
                          ".s_axis_tdata(prod_M_AXIS_TDATA)", ".m_axis_tdata(cc_M_AXIS_TDATA)",
                          // unconnected inputs get their default value
                          ".s_axis_aresetn(1'd1)",
                          // the hierarchy is a module of its own: the instance paths are Vivado's (grp/cons)
                          "design_1_grp_imp grp (", ".cc_M_AXIS_TDATA(cc_M_AXIS_TDATA)", ".led(led)",
                          "module design_1_grp_imp (", "input  wire [0:0] clk_b", "input  wire [7:0] cc_M_AXIS_TDATA",
                          "output wire [0:0] cc_M_AXIS_TREADY", "output wire [7:0] led",
                          "consumer cons (", ".clk(clk_b)", ".s_tdata(cc_M_AXIS_TDATA)", ".q(led)",
                          ".stat_o(STAT_tri_o)"})
        CHECK(has(v, s));
    // negative: nothing is driven twice or left without a driver; a net only inside the hierarchy is not its port
    CHECK(b.notes.empty());
    CHECK_FALSE(has(v, "assign"));
    CHECK_FALSE(has(v, ".clk_a(clk_a)"));
}

TEST_CASE("Block design: parts of a port in different interfaces, unconnected outputs", "[frontend][bd]") {
    test::TempDir dir;
    // a two-slot IP whose slots share the concatenated port (as the AXI crossbar)
    dir.write("d/ip/d_sw_0/d_sw_0.xci", R"({ "ip_inst": { "xci_name": "d_sw_0",
      "component_reference": "xilinx.com:ip:axis_switch:1.1",
      "boundary": {
        "ports": { "aclk": [ { "direction": "in" } ],
                   "s_axis_tdata": [ { "direction": "in", "size_left": "15", "size_right": "0" } ],
                   "m_axis_tdata": [ { "direction": "out", "size_left": "7", "size_right": "0" } ],
                   "m_axis_tvalid": [ { "direction": "out" } ] },
        "interfaces": {
          "S00_AXIS": { "port_maps": { "TDATA": [ { "physical_name": "s_axis_tdata", "physical_left": "7", "physical_right": "0" } ] } },
          "S01_AXIS": { "port_maps": { "TDATA": [ { "physical_name": "s_axis_tdata", "physical_left": "15", "physical_right": "8" } ] } },
          "M00_AXIS": { "port_maps": { "TDATA": [ { "physical_name": "m_axis_tdata" } ] } } } } } })");
    const std::string ref = R"("vlnv": "xilinx.com:module_ref:src:1.0", "reference_info": { "ref_name": "src" },
        "interface_ports": { "M": { "port_maps": { "TDATA": { "physical_name": "d", "direction": "O", "left": "7", "right": "0" } } } })";
    const std::string bd = R"({ "design": { "design_info": { "name": "d" },
      "ports": { "clk": { "direction": "I" } },
      "components": {
        "a": { )" + ref + R"( },
        "b": { )" + ref + R"( },
        "sw": { "vlnv": "xilinx.com:ip:axis_switch:1.1", "xci_name": "d_sw_0" } },
      "interface_nets": {
        "a_M": { "interface_ports": [ "a/M", "sw/S00_AXIS" ] },
        "b_M": { "interface_ports": [ "b/M", "sw/S01_AXIS" ] } },
      "nets": { "clk_1": { "ports": [ "clk", "sw/aclk" ] } } } })";
    auto r = frontend::blockDesignFromText(bd, dir.path() / "d");
    REQUIRE(r);
    const auto& v = r.value().verilog;
    INFO(v);
    CHECK(has(v, ".s_axis_tdata({b_M_TDATA, a_M_TDATA})"));
    CHECK(has(v, ".m_axis_tdata()"));
    CHECK(has(v, "src a ("));
    CHECK(has(v, ".d(a_M_TDATA)"));
    // negative: one slot only: the other half of the port is tied to 0
    const std::string one = R"({ "design": { "design_info": { "name": "d" }, "components": {
        "b": { )" + ref + R"( }, "sw": { "vlnv": "xilinx.com:ip:axis_switch:1.1", "xci_name": "d_sw_0" } },
      "interface_nets": { "b_M": { "interface_ports": [ "b/M", "sw/S01_AXIS" ] } } } })";
    auto r1 = frontend::blockDesignFromText(one, dir.path() / "d");
    REQUIRE(r1);
    CHECK(has(r1.value().verilog, ".s_axis_tdata({b_M_TDATA, 8'd0})"));
    CHECK(has(r1.value().verilog, ".aclk(1'd0)"));
}

TEST_CASE("Block design: errors", "[frontend][bd]") {
    test::TempDir dir;
    auto bad = frontend::blockDesignFromText("{ not json", dir.path());
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "FE-BD-PARSE");
    auto noName = frontend::blockDesignFromText(R"({ "design": { "components": {} } })", dir.path());
    REQUIRE_FALSE(noName);
    CHECK(noName.error().code == "FE-BD-PARSE");
    // a cell whose ports are not known (no output products next to the .bd)
    auto missing = frontend::blockDesignFromText(
        R"({ "design": { "design_info": { "name": "d" }, "components": {
             "x": { "vlnv": "xilinx.com:ip:axi_gpio:2.0", "xci_name": "d_x_0" } } } })",
        dir.path());
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "FE-BD-CELL");
    CHECK(has(missing.error().message, "'x'"));
    CHECK(frontend::isBlockDesignFile("a/b/design_1.bd"));
    CHECK_FALSE(frontend::isBlockDesignFile("a/b/design_1.bxml"));
}

TEST_CASE("Block design: analyzed with the IP models; the clock converter separates the clocks", "[frontend][bd]") {
    test::TempDir dir;
    const auto r = run(project(dir, true));
    const auto* bd = find(*r, "FE-BD-MODULE");
    REQUIRE(bd);
    CHECK(has(bd->message, "module 'design_1' with 3 cell(s), 1 hierarchical cell(s), 1 IP configuration(s)"));
    const auto* ip = find(*r, "FE-XCI-MODEL");
    REQUIRE(ip);
    CHECK(has(ip->message, "AXI4-Stream clock converter, s_axis_aclk to m_axis_aclk, asynchronous clocks"));
    CHECK_FALSE(r->hasErrors());
    REQUIRE(r->cdc);
    CHECK(r->cdc->paths.empty());
    // negative: the producer straight into the consumer: the interface net carries the crossing
    test::TempDir dir2;
    const auto r2 = run(project(dir2, false));
    REQUIRE(r2->cdc);
    REQUIRE_FALSE(r2->cdc->paths.empty());
    const bool toConsumer = std::ranges::any_of(r2->cdc->paths, [](const auto& p) {
        return p.dstName.find("grp.cons.") != std::string::npos;
    });
    CHECK(toConsumer);
    // the instance paths are Vivado's: a constraint written for the Vivado netlist matches
    auto cfg3 = project(dir2, false);
    cfg3.constraints.push_back(dir2.write("fp.xdc", "set_false_path -to [get_cells grp/cons/r_reg*]\n"));
    const auto r3 = run(cfg3);
    CHECK(find(*r3, "CON-UNMATCHED") == nullptr);
    // negative: the flattened name of earlier versions matches nothing
    auto cfg4 = project(dir2, false);
    cfg4.constraints.push_back(dir2.write("fp2.xdc", "set_false_path -to [get_cells grp_cons/r_reg*]\n"));
    const auto r4 = run(cfg4);
    CHECK(find(*r4, "CON-UNMATCHED") != nullptr);
}

TEST_CASE("Block design: nested hierarchies keep Vivado's instance paths", "[frontend][bd]") {
    test::TempDir dir;
    const std::string ref = R"("vlnv": "xilinx.com:module_ref:cell:1.0", "reference_info": { "ref_name": "cell" },
        "ports": { "i": { "direction": "I" }, "o": { "direction": "O" } })";
    // h1/h2/a drives h1/b (inside h1) and c (outside); b drives nothing
    const std::string bd = R"({ "design": { "design_info": { "name": "d" },
      "components": {
        "c": { )" + ref + R"( },
        "h1": {
          "ports": { "X": { "direction": "O" } },
          "components": {
            "b": { )" + ref + R"( },
            "h2": { "ports": { "Y": { "direction": "O" } },
                    "components": { "a": { )" + ref + R"( } },
                    "nets": { "a_o": { "ports": [ "a/o", "Y" ] } } } },
          "nets": { "h2_Y": { "ports": [ "h2/Y", "b/i", "X" ] } } } },
      "nets": { "h1_X": { "ports": [ "h1/X", "c/i" ] } } } })";
    auto r = frontend::blockDesignFromText(bd, dir.path());
    REQUIRE(r);
    const auto& v = r.value().verilog;
    INFO(v);
    for (const char* s : {"module d (", "d_h1_imp h1 (", ".h1_X(h1_X)", "cell c (", ".i(h1_X)",
                          "module d_h1_imp (\n  output wire [0:0] h1_X\n);", "d_h1_h2_imp h2 (", "cell b (",
                          "module d_h1_h2_imp (\n  output wire [0:0] h1_X\n);", "cell a (", ".o(h1_X)"})
        CHECK(has(v, s));
    CHECK(r.value().hierarchies == 2);
    // negative: b's unconnected output is no port of h1, and no leaf is named after its path
    CHECK_FALSE(has(v, "h1_h2_a"));
    CHECK_FALSE(has(v, "h1_b ("));
}

namespace {

// an IP whose ports and interface come from its JSON .xci (the boundary)
std::string xmlCellXci(const std::string& inst, const std::string& dir) {
    const std::string d = dir == "out" ? "out" : "in";
    return R"({ "ip_inst": { "xci_name": ")" + inst + R"(", "component_reference": "xilinx.com:ip:my_ip:1.0",
      "boundary": {
        "ports": { "clk": [ { "direction": "in" } ],
                   "tdata": [ { "direction": ")" + d + R"(", "size_left": "7", "size_right": "0" } ] },
        "interfaces": { "AXIS": { "port_maps": { "TDATA": [ { "physical_name": "tdata" } ] } } } } } })";
}

// an XML block design (Vivado 2018.2 and earlier) with a hierarchy:
// src (top level) -> h/dst, the clock from the design port clk, the result to a design port
const char* kXmlBd = R"(<?xml version="1.0" encoding="UTF-8" standalone="no" ?>
<bd:repository xmlns:bd="http://www.xilinx.com/bd" bd:tool_version="2018.2" bd:top="xd" bd:version="1.00.a">
  <spirit:component xmlns:spirit="http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009">
    <spirit:vendor>xilinx.com</spirit:vendor>
    <spirit:library>BlockDiagram</spirit:library>
    <spirit:name>xd</spirit:name>
    <spirit:version>1.00.a</spirit:version>
    <spirit:model>
      <spirit:views><spirit:view><spirit:name>BlockDiagram</spirit:name>
        <spirit:hierarchyRef spirit:library="BlockDiagram" spirit:name="xd_imp" spirit:vendor="xilinx.com" spirit:version="1.00.a"/>
      </spirit:view></spirit:views>
      <spirit:ports>
        <spirit:port><spirit:name>clk</spirit:name><spirit:wire><spirit:direction>in</spirit:direction></spirit:wire></spirit:port>
        <spirit:port><spirit:name>q</spirit:name><spirit:wire><spirit:direction>out</spirit:direction>
          <spirit:vector><spirit:left spirit:format="long">7</spirit:left><spirit:right spirit:format="long">0</spirit:right></spirit:vector>
        </spirit:wire></spirit:port>
      </spirit:ports>
    </spirit:model>
  </spirit:component>
  <spirit:design xmlns:spirit="http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009">
    <spirit:vendor>xilinx.com</spirit:vendor>
    <spirit:library>BlockDiagram</spirit:library>
    <spirit:name>xd_imp</spirit:name>
    <spirit:version>1.00.a</spirit:version>
    <spirit:componentInstances>
      <spirit:componentInstance>
        <spirit:instanceName>src</spirit:instanceName>
        <spirit:componentRef spirit:library="ip" spirit:name="my_ip" spirit:vendor="xilinx.com" spirit:version="1.0"/>
        <spirit:configurableElementValues>
          <spirit:configurableElementValue spirit:referenceId="bd:xciName">xd_src_0</spirit:configurableElementValue>
        </spirit:configurableElementValues>
      </spirit:componentInstance>
      <spirit:componentInstance>
        <spirit:instanceName>h</spirit:instanceName>
        <spirit:componentRef spirit:library="BlockDiagram/xd_imp" spirit:name="h" spirit:vendor="xilinx.com" spirit:version="1.00.a"/>
      </spirit:componentInstance>
    </spirit:componentInstances>
    <spirit:interconnections>
      <spirit:interconnection>
        <spirit:name>src_AXIS</spirit:name>
        <spirit:activeInterface spirit:busRef="AXIS" spirit:componentRef="src"/>
        <spirit:activeInterface spirit:busRef="S" spirit:componentRef="h"/>
      </spirit:interconnection>
    </spirit:interconnections>
    <spirit:adHocConnections>
      <spirit:adHocConnection>
        <spirit:name>clk_1</spirit:name>
        <spirit:externalPortReference spirit:portRef="clk"/>
        <spirit:internalPortReference spirit:componentRef="src" spirit:portRef="clk"/>
        <spirit:internalPortReference spirit:componentRef="h" spirit:portRef="CLK"/>
      </spirit:adHocConnection>
    </spirit:adHocConnections>
  </spirit:design>
  <spirit:component xmlns:spirit="http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009">
    <spirit:vendor>xilinx.com</spirit:vendor>
    <spirit:library>BlockDiagram/xd_imp</spirit:library>
    <spirit:name>h</spirit:name>
    <spirit:version>1.00.a</spirit:version>
    <spirit:busInterfaces><spirit:busInterface><spirit:name>S</spirit:name></spirit:busInterface></spirit:busInterfaces>
    <spirit:model>
      <spirit:views><spirit:view><spirit:name>BlockDiagram</spirit:name>
        <spirit:hierarchyRef spirit:library="BlockDiagram/xd_imp" spirit:name="h_imp" spirit:vendor="xilinx.com" spirit:version="1.00.a"/>
      </spirit:view></spirit:views>
      <spirit:ports>
        <spirit:port><spirit:name>CLK</spirit:name><spirit:wire><spirit:direction>in</spirit:direction></spirit:wire></spirit:port>
      </spirit:ports>
    </spirit:model>
  </spirit:component>
  <spirit:design xmlns:spirit="http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009">
    <spirit:vendor>xilinx.com</spirit:vendor>
    <spirit:library>BlockDiagram/xd_imp</spirit:library>
    <spirit:name>h_imp</spirit:name>
    <spirit:version>1.00.a</spirit:version>
    <spirit:componentInstances>
      <spirit:componentInstance>
        <spirit:instanceName>dst</spirit:instanceName>
        <spirit:componentRef spirit:library="ip" spirit:name="my_ip" spirit:vendor="xilinx.com" spirit:version="1.0"/>
        <spirit:configurableElementValues>
          <spirit:configurableElementValue spirit:referenceId="bd:xciName">xd_dst_0</spirit:configurableElementValue>
        </spirit:configurableElementValues>
      </spirit:componentInstance>
    </spirit:componentInstances>
    <spirit:hierConnections>
      <spirit:hierConnection spirit:interfaceRef="S/S_1">
        <spirit:activeInterface spirit:busRef="AXIS" spirit:componentRef="dst"/>
      </spirit:hierConnection>
    </spirit:hierConnections>
    <spirit:adHocConnections>
      <spirit:adHocConnection>
        <spirit:name>CLK_1</spirit:name>
        <spirit:externalPortReference spirit:portRef="CLK"/>
        <spirit:internalPortReference spirit:componentRef="dst" spirit:portRef="clk"/>
      </spirit:adHocConnection>
    </spirit:adHocConnections>
  </spirit:design>
</bd:repository>
)";

} // namespace

TEST_CASE("Block design: the XML format of Vivado 2018.2 and earlier", "[frontend][bd]") {
    test::TempDir dir;
    dir.write("xd/ip/xd_src_0/xd_src_0.xci", xmlCellXci("xd_src_0", "out"));
    dir.write("xd/ip/xd_dst_0/xd_dst_0.xci", xmlCellXci("xd_dst_0", "in"));
    // a byte order mark, as Vivado writes it
    auto r = frontend::blockDesignFromText(std::string("\xEF\xBB\xBF") + kXmlBd, dir.path() / "xd");
    REQUIRE(r);
    const auto& b = r.value();
    const auto& v = b.verilog;
    INFO(v);
    CHECK(b.name == "xd");
    CHECK(b.cells == 2);
    CHECK(b.hierarchies == 1);
    CHECK(b.ipFiles.size() == 2);
    for (const char* s : {"module xd (", "input  wire clk", "output wire [7:0] q",
                          // the interconnection and the hier connection: one net from src into h/dst
                          "wire [7:0] src_AXIS_TDATA;", "xd_src_0 src (", ".tdata(src_AXIS_TDATA)", "xd_h_imp h (",
                          "module xd_h_imp (", "input  wire [7:0] src_AXIS_TDATA", "xd_dst_0 dst (",
                          // the ad-hoc connections through the hierarchy's port CLK
                          ".clk(clk)"})
        CHECK(has(v, s));
    // negative: a repository without the top component, and XML that is no block design
    auto noTop = frontend::blockDesignFromText(
        "<bd:repository bd:top=\"zz\"><spirit:component><spirit:library>BlockDiagram</spirit:library>"
        "<spirit:name>xd</spirit:name></spirit:component></bd:repository>",
        dir.path());
    REQUIRE_FALSE(noTop);
    CHECK(noTop.error().code == "FE-BD-PARSE");
    CHECK(has(noTop.error().message, "no top component 'zz'"));
    auto notBd = frontend::blockDesignFromText("<?xml version=\"1.0\"?><spirit:component/>", dir.path());
    REQUIRE_FALSE(notBd);
    CHECK(has(notBd.error().message, "bd:repository"));
}
