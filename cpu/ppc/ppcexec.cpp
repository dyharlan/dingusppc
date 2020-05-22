/*
DingusPPC - The Experimental PowerPC Macintosh emulator
Copyright (C) 2018-20 divingkatae and maximum
                      (theweirdo)     spatium

(Contact divingkatae#1017 or powermax#2286 on Discord for more info)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <thirdparty/loguru/loguru.hpp>
#include <stdio.h>
#include <string>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <chrono>
#include <setjmp.h>

#include "ppcemu.h"
#include "ppcmmu.h"

using namespace std;

MemCtrlBase *mem_ctrl_instance = 0;

bool power_on = 1;

SetPRS ppc_state;

bool grab_branch;
bool grab_exception;
bool grab_return;
bool grab_breakpoint;

bool rc_flag = 0;
bool oe_flag = 0;

uint32_t ppc_cur_instruction; //Current instruction for the PPC
uint32_t ppc_effective_address;
uint32_t ppc_next_instruction_address; //Used for branching, setting up the NIA

BB_end_kind bb_kind; /* basic block end */

uint64_t timebase_counter; /* internal timebase counter */

clock_t clock_test_begin; //Used to make sure the TBR does not increment so quickly.

/** Opcode lookup tables. */

/** Primary opcode (bits 0...5) lookup table. */
static PPCOpcode OpcodeGrabber[] = {
    ppc_illegalop, ppc_illegalop, ppc_illegalop, ppc_twi,       ppc_opcode4,
    ppc_illegalop, ppc_illegalop, ppc_mulli,     ppc_subfic,    power_dozi,
    ppc_cmpli,     ppc_cmpi,      ppc_addic,     ppc_addicdot,  ppc_addi,
    ppc_addis,     ppc_opcode16,  ppc_sc,        ppc_opcode18,  ppc_opcode19,
    ppc_rlwimi,    ppc_rlwinm,    power_rlmi,    ppc_rlwnm,     ppc_ori,
    ppc_oris,      ppc_xori,      ppc_xoris,     ppc_andidot,   ppc_andisdot,
    ppc_illegalop, ppc_opcode31,  ppc_lwz,       ppc_lwzu,      ppc_lbz,
    ppc_lbzu,      ppc_stw,       ppc_stwu,      ppc_stb,       ppc_stbu,
    ppc_lhz,       ppc_lhzu,      ppc_lha,       ppc_lhau,      ppc_sth,
    ppc_sthu,      ppc_lmw,       ppc_stmw,      ppc_lfs,       ppc_lfsu,
    ppc_lfd,       ppc_lfdu,      ppc_stfs,      ppc_stfsu,     ppc_stfd,
    ppc_stfdu,     ppc_psq_l,     ppc_psq_lu,    ppc_illegalop, ppc_illegalop,
    ppc_psq_st,    ppc_psq_stu,   ppc_illegalop, ppc_opcode63
};

/** Lookup tables for branch instructions. */
static PPCOpcode SubOpcode16Grabber[] = {
    ppc_bc, ppc_bcl, ppc_bca, ppc_bcla
};

static PPCOpcode SubOpcode18Grabber[] = {
    ppc_b, ppc_bl, ppc_ba, ppc_bla
};

/** General conditional register instructions decoding table. */

static PPCOpcode SubOpcode31Grabber[2048] = { ppc_illegalsubop31};

/** Single-precision floating-point instructions decoding table. */
static std::unordered_map<uint16_t, PPCOpcode> SubOpcode59Grabber = {
    {  36, &ppc_fdivs},       {  37, &ppc_fdivsdot},    {  40, &ppc_fsubs},
    {  41, &ppc_fsubsdot},    {  42, &ppc_fadds},       {  43, &ppc_faddsdot},
    {  44, &ppc_fsqrts},      {  45, &ppc_fsqrtsdot},   {  48, &ppc_fres},
    {  49, &ppc_fresdot},     {  50, &ppc_fmults},      {  51, &ppc_fmultsdot},
    {  56, &ppc_fmsubs},      {  57, &ppc_fmsubsdot},   {  58, &ppc_fmadds},
    {  59, &ppc_fmaddsdot},   {  60, &ppc_fnmsubs},     {  61, &ppc_fnmsubsdot},
    {  62, &ppc_fnmadds},     {  63, &ppc_fnmaddsdot},  { 114, &ppc_fmults},
    { 115, &ppc_fmultsdot},   { 120, &ppc_fmsubs},      { 121, &ppc_fmsubsdot},
    { 122, &ppc_fmadds},      { 123, &ppc_fmadds},      { 124, &ppc_fnmsubs},
    { 125, &ppc_fnmsubsdot},  { 126, &ppc_fnmadds},     { 127, &ppc_fnmaddsdot},
    { 178, &ppc_fmults},      { 179, &ppc_fmultsdot},   { 184, &ppc_fmsubs},
    { 185, &ppc_fmsubsdot},   { 186, &ppc_fmadds},      { 187, &ppc_fmaddsdot},
    { 188, &ppc_fnmsubs},     { 189, &ppc_fnmsubsdot},  { 190, &ppc_fnmadds},
    { 191, &ppc_fnmaddsdot},  { 242, &ppc_fmults},      { 243, &ppc_fmultsdot},
    { 248, &ppc_fmsubs},      { 249, &ppc_fmsubsdot},   { 250, &ppc_fmadds},
    { 251, &ppc_fmaddsdot},   { 252, &ppc_fnmsubs},     { 253, &ppc_fnmsubsdot},
    { 254, &ppc_fnmadds},     { 255, &ppc_fnmaddsdot},  { 306, &ppc_fmults},
    { 307, &ppc_fmultsdot},   { 312, &ppc_fmsubs},      { 313, &ppc_fmsubsdot},
    { 314, &ppc_fmadds},      { 315, &ppc_fmaddsdot},   { 316, &ppc_fnmsubs},
    { 317, &ppc_fnmsubsdot},  { 318, &ppc_fnmadds},     { 319, &ppc_fnmaddsdot},
    { 370, &ppc_fmults},      { 371, &ppc_fmultsdot},   { 376, &ppc_fmsubs},
    { 377, &ppc_fmsubsdot},   { 378, &ppc_fmadds},      { 379, &ppc_fmaddsdot},
    { 380, &ppc_fnmsubs},     { 381, &ppc_fnmsubsdot},  { 382, &ppc_fnmadds},
    { 383, &ppc_fnmaddsdot},  { 434, &ppc_fmults},      { 435, &ppc_fmultsdot},
    { 440, &ppc_fmsubs},      { 441, &ppc_fmsubsdot},   { 442, &ppc_fmadds},
    { 443, &ppc_fmaddsdot},   { 444, &ppc_fnmsubs},     { 445, &ppc_fnmsubsdot},
    { 446, &ppc_fnmadds},     { 447, &ppc_fnmaddsdot},  { 498, &ppc_fmults},
    { 499, &ppc_fmultsdot},   { 504, &ppc_fmsubs},      { 505, &ppc_fmsubsdot},
    { 506, &ppc_fmadds},      { 507, &ppc_fmaddsdot},   { 508, &ppc_fnmsubs},
    { 509, &ppc_fnmsubsdot},  { 510, &ppc_fnmadds},     { 511, &ppc_fnmaddsdot},
    { 562, &ppc_fmults},      { 563, &ppc_fmultsdot},   { 568, &ppc_fmsubs},
    { 569, &ppc_fmsubsdot},   { 570, &ppc_fmadds},      { 571, &ppc_fmaddsdot},
    { 572, &ppc_fnmsubs},     { 573, &ppc_fnmsubsdot},  { 574, &ppc_fnmadds},
    { 575, &ppc_fnmaddsdot},  { 626, &ppc_fmults},      { 627, &ppc_fmultsdot},
    { 632, &ppc_fmsubs},      { 633, &ppc_fmsubsdot},   { 634, &ppc_fmadds},
    { 635, &ppc_fmaddsdot},   { 636, &ppc_fnmsubs},     { 637, &ppc_fnmsubsdot},
    { 638, &ppc_fnmadds},     { 639, &ppc_fnmaddsdot},  { 690, &ppc_fmults},
    { 691, &ppc_fmultsdot},   { 696, &ppc_fmsubs},      { 697, &ppc_fmsubsdot},
    { 698, &ppc_fmadds},      { 699, &ppc_fmaddsdot},   { 700, &ppc_fnmsubs},
    { 701, &ppc_fnmsubsdot},  { 702, &ppc_fnmadds},     { 703, &ppc_fnmaddsdot},
    { 754, &ppc_fmults},      { 755, &ppc_fmultsdot},   { 760, &ppc_fmsubs},
    { 761, &ppc_fmsubsdot},   { 762, &ppc_fmadds},      { 763, &ppc_fmaddsdot},
    { 764, &ppc_fnmsubs},     { 765, &ppc_fnmsubsdot},  { 766, &ppc_fnmadds},
    { 767, &ppc_fnmaddsdot},  { 818, &ppc_fmults},      { 819, &ppc_fmultsdot},
    { 824, &ppc_fmsubs},      { 825, &ppc_fmsubsdot},   { 826, &ppc_fmadds},
    { 827, &ppc_fmaddsdot},   { 828, &ppc_fnmsubs},     { 829, &ppc_fnmsubsdot},
    { 830, &ppc_fnmadds},     { 831, &ppc_fnmaddsdot},  { 882, &ppc_fmults},
    { 883, &ppc_fmultsdot},   { 888, &ppc_fmsubs},      { 889, &ppc_fmsubsdot},
    { 890, &ppc_fmadds},      { 891, &ppc_fmaddsdot},   { 892, &ppc_fnmsubs},
    { 893, &ppc_fnmsubsdot},  { 894, &ppc_fnmadds},     { 895, &ppc_fnmaddsdot},
    { 946, &ppc_fmults},      { 947, &ppc_fmultsdot},   { 952, &ppc_fmsubs},
    { 953, &ppc_fmsubsdot},   { 954, &ppc_fmadds},      { 955, &ppc_fmaddsdot},
    { 957, &ppc_fnmsubs},     { 958, &ppc_fnmsubsdot},  { 958, &ppc_fnmadds},
    { 959, &ppc_fnmaddsdot},  {1010, &ppc_fmults},      {1011, &ppc_fmultsdot},
    {1016, &ppc_fmsubs},      {1017, &ppc_fmsubsdot},   {1018, &ppc_fmadds},
    {1019, &ppc_fmaddsdot},   {1020, &ppc_fnmsubs},     {1021, &ppc_fnmsubsdot},
    {1022, &ppc_fnmadds},     {1023, &ppc_fnmaddsdot},  {1074, &ppc_fmults},
    {1075, &ppc_fmultsdot},   {1080, &ppc_fmsubs},      {1081, &ppc_fmsubsdot},
    {1082, &ppc_fmadds},      {1083, &ppc_fmaddsdot},   {1084, &ppc_fnmsubs},
    {1085, &ppc_fnmsubsdot},  {1086, &ppc_fnmadds},     {1087, &ppc_fnmaddsdot},
    {1138, &ppc_fmults},      {1139, &ppc_fmultsdot},   {1144, &ppc_fmsubs},
    {1145, &ppc_fmsubsdot},   {1146, &ppc_fmadds},      {1147, &ppc_fmaddsdot},
    {1148, &ppc_fnmsubs},     {1149, &ppc_fnmsubsdot},  {1150, &ppc_fnmadds},
    {1151, &ppc_fnmaddsdot},  {1202, &ppc_fmults},      {1203, &ppc_fmultsdot},
    {1208, &ppc_fmsubs},      {1209, &ppc_fmsubsdot},   {1210, &ppc_fmadds},
    {1211, &ppc_fmaddsdot},   {1212, &ppc_fnmsubs},     {1213, &ppc_fnmsubsdot},
    {1214, &ppc_fnmadds},     {1215, &ppc_fnmaddsdot},  {1266, &ppc_fmults},
    {1267, &ppc_fmultsdot},   {1272, &ppc_fmsubs},      {1273, &ppc_fmsubsdot},
    {1274, &ppc_fmadds},      {1275, &ppc_fmaddsdot},   {1276, &ppc_fnmsubs},
    {1277, &ppc_fnmsubsdot},  {1278, &ppc_fnmadds},     {1279, &ppc_fnmaddsdot},
    {1330, &ppc_fmults},      {1331, &ppc_fmultsdot},   {1336, &ppc_fmsubs},
    {1337, &ppc_fmsubsdot},   {1338, &ppc_fmadds},      {1339, &ppc_fmaddsdot},
    {1340, &ppc_fnmsubs},     {1341, &ppc_fnmsubsdot},  {1342, &ppc_fnmadds},
    {1343, &ppc_fnmaddsdot},  {1394, &ppc_fmults},      {1395, &ppc_fmultsdot},
    {1400, &ppc_fmsubs},      {1401, &ppc_fmsubsdot},   {1402, &ppc_fmadds},
    {1403, &ppc_fmaddsdot},   {1404, &ppc_fnmsubs},     {1405, &ppc_fnmsubsdot},
    {1406, &ppc_fnmadds},     {1407, &ppc_fnmaddsdot},  {1458, &ppc_fmults},
    {1459, &ppc_fmultsdot},   {1464, &ppc_fmsubs},      {1465, &ppc_fmsubsdot},
    {1466, &ppc_fmadds},      {1467, &ppc_fmaddsdot},   {1468, &ppc_fnmsubs},
    {1469, &ppc_fnmsubsdot},  {1470, &ppc_fnmadds},     {1471, &ppc_fnmaddsdot},
    {1522, &ppc_fmults},      {1523, &ppc_fmultsdot},   {1528, &ppc_fmsubs},
    {1529, &ppc_fmsubsdot},   {1530, &ppc_fmadds},      {1531, &ppc_fmaddsdot},
    {1532, &ppc_fnmsubs},     {1533, &ppc_fnmsubsdot},  {1534, &ppc_fnmadds},
    {1535, &ppc_fnmaddsdot},  {1586, &ppc_fmults},      {1587, &ppc_fmultsdot},
    {1592, &ppc_fmsubs},      {1593, &ppc_fmsubsdot},   {1594, &ppc_fmadds},
    {1595, &ppc_fmaddsdot},   {1596, &ppc_fnmsubs},     {1597, &ppc_fnmsubsdot},
    {1598, &ppc_fnmadds},     {1599, &ppc_fnmaddsdot},  {1650, &ppc_fmults},
    {1651, &ppc_fmultsdot},   {1656, &ppc_fmsubs},      {1657, &ppc_fmsubsdot},
    {1658, &ppc_fmadds},      {1659, &ppc_fmaddsdot},   {1660, &ppc_fnmsubs},
    {1661, &ppc_fnmsubsdot},  {1662, &ppc_fnmadds},     {1663, &ppc_fnmaddsdot},
    {1714, &ppc_fmults},      {1715, &ppc_fmultsdot},   {1720, &ppc_fmsubs},
    {1721, &ppc_fmsubsdot},   {1722, &ppc_fmadds},      {1723, &ppc_fmaddsdot},
    {1724, &ppc_fnmsubs},     {1725, &ppc_fnmsubsdot},  {1726, &ppc_fnmadds},
    {1727, &ppc_fnmaddsdot},  {1778, &ppc_fmults},      {1779, &ppc_fmultsdot},
    {1784, &ppc_fmsubs},      {1785, &ppc_fmsubsdot},   {1786, &ppc_fmadds},
    {1787, &ppc_fmaddsdot},   {1788, &ppc_fnmsubs},     {1789, &ppc_fnmsubsdot},
    {1790, &ppc_fnmadds},     {1791, &ppc_fnmaddsdot},  {1842, &ppc_fmults},
    {1843, &ppc_fmultsdot},   {1848, &ppc_fmsubs},      {1849, &ppc_fmsubsdot},
    {1850, &ppc_fmadds},      {1851, &ppc_fmaddsdot},   {1852, &ppc_fnmsubs},
    {1853, &ppc_fnmsubsdot},  {1854, &ppc_fnmadds},     {1855, &ppc_fnmaddsdot},
    {1906, &ppc_fmults},      {1907, &ppc_fmultsdot},   {1912, &ppc_fmsubs},
    {1913, &ppc_fmsubsdot},   {1914, &ppc_fmadds},      {1915, &ppc_fmaddsdot},
    {1916, &ppc_fnmsubs},     {1917, &ppc_fnmsubsdot},  {1918, &ppc_fnmadds},
    {1919, &ppc_fnmaddsdot},  {1970, &ppc_fmults},      {1971, &ppc_fmultsdot},
    {1976, &ppc_fmsubs},      {1977, &ppc_fmsubsdot},   {1978, &ppc_fmadds},
    {1979, &ppc_fmaddsdot},   {1980, &ppc_fnmsubs},     {1981, &ppc_fnmsubsdot},
    {1982, &ppc_fnmadds},     {1983, &ppc_fnmaddsdot},  {2034, &ppc_fmults},
    {2035, &ppc_fmultsdot},   {2040, &ppc_fmsubs},      {2041, &ppc_fmsubsdot},
    {2042, &ppc_fmadds},      {2043, &ppc_fmaddsdot},   {2044, &ppc_fnmsubs},
    {2045, &ppc_fnmsubsdot},  {2046, &ppc_fnmadds},     {2047, &ppc_fnmaddsdot}
};

/** Double-precision floating-point instructions decoding table. */
static std::unordered_map<uint16_t, PPCOpcode> SubOpcode63Grabber = {
    {   0, &ppc_fcmpu},      {  24, &ppc_frsp},       {  25, &ppc_frspdot},
    {  28, &ppc_fctiw},      {  29, &ppc_fctiwdot},   {  30, &ppc_fctiwz},
    {  31, &ppc_fctiwzdot},  {  36, &ppc_fdiv},       {  37, &ppc_fdivdot},
    {  40, &ppc_fsub},       {  41, &ppc_fsubdot},    {  42, &ppc_fadd},
    {  43, &ppc_fadddot},    {  44, &ppc_fsqrt},      {  45, &ppc_fsqrtdot},
    {  46, &ppc_fsel},       {  47, &ppc_fseldot},    {  50, &ppc_fmult},
    {  51, &ppc_fmultdot},   {  52, &ppc_frsqrte},    {  53, &ppc_frsqrtedot},
    {  56, &ppc_fmsub},      {  57, &ppc_fmsubdot},   {  58, &ppc_fmadd},
    {  59, &ppc_fmadddot},   {  60, &ppc_fnmsub},     {  61, &ppc_fnmsubdot},
    {  62, &ppc_fnmadd},     {  63, &ppc_fnmadddot},  {  64, &ppc_fcmpo},
    {  76, &ppc_mtfsb1},     {  77, &ppc_mtfsb1dot},  {  80, &ppc_fneg},
    {  81, &ppc_fnegdot},    { 110, &ppc_fsel},       { 111, &ppc_fseldot},
    { 114, &ppc_fmult},      { 115, &ppc_fmultdot},   { 120, &ppc_fmsub},
    { 121, &ppc_fmsubdot},   { 122, &ppc_fmadd},      { 123, &ppc_fmadd},
    { 124, &ppc_fnmsub},     { 125, &ppc_fnmsubdot},  { 126, &ppc_fnmadd},
    { 127, &ppc_fnmadddot},  { 128, &ppc_mcrfs},      { 140, &ppc_mtfsb0},
    { 141, &ppc_mtfsb0dot},  { 144, &ppc_fmr},        { 174, &ppc_fsel},
    { 175, &ppc_fseldot},    { 178, &ppc_fmult},      { 179, &ppc_fmultdot},
    { 184, &ppc_fmsub},      { 185, &ppc_fmsubdot},   { 186, &ppc_fmadd},
    { 187, &ppc_fmadddot},   { 188, &ppc_fnmsub},     { 189, &ppc_fnmsubdot},
    { 190, &ppc_fnmadd},     { 191, &ppc_fnmadddot},  { 238, &ppc_fsel},
    { 239, &ppc_fseldot},    { 242, &ppc_fmult},      { 243, &ppc_fmultdot},
    { 248, &ppc_fmsub},      { 249, &ppc_fmsubdot},   { 250, &ppc_fmadd},
    { 251, &ppc_fmadddot},   { 252, &ppc_fnmsub},     { 253, &ppc_fnmsubdot},
    { 254, &ppc_fnmadd},     { 255, &ppc_fnmadddot},  { 268, &ppc_mtfsfi},
    { 272, &ppc_fnabs},      { 273, &ppc_fnabsdot},   { 302, &ppc_fsel},
    { 303, &ppc_fseldot},    { 306, &ppc_fmult},      { 307, &ppc_fmultdot},
    { 312, &ppc_fmsub},      { 313, &ppc_fmsubdot},   { 314, &ppc_fmadd},
    { 315, &ppc_fmadddot},   { 316, &ppc_fnmsub},     { 317, &ppc_fnmsubdot},
    { 318, &ppc_fnmadd},     { 319, &ppc_fnmadddot},  { 366, &ppc_fsel},
    { 367, &ppc_fseldot},    { 370, &ppc_fmult},      { 371, &ppc_fmultdot},
    { 376, &ppc_fmsub},      { 377, &ppc_fmsubdot},   { 378, &ppc_fmadd},
    { 379, &ppc_fmadddot},   { 380, &ppc_fnmsub},     { 381, &ppc_fnmsubdot},
    { 382, &ppc_fnmadd},     { 383, &ppc_fnmadddot},  { 430, &ppc_fsel},
    { 431, &ppc_fseldot},    { 434, &ppc_fmult},      { 435, &ppc_fmultdot},
    { 440, &ppc_fmsub},      { 441, &ppc_fmsubdot},   { 442, &ppc_fmadd},
    { 443, &ppc_fmadddot},   { 444, &ppc_fnmsub},     { 445, &ppc_fnmsubdot},
    { 446, &ppc_fnmadd},     { 447, &ppc_fnmadddot},  { 494, &ppc_fsel},
    { 495, &ppc_fseldot},    { 498, &ppc_fmult},      { 499, &ppc_fmultdot},
    { 504, &ppc_fmsub},      { 505, &ppc_fmsubdot},   { 506, &ppc_fmadd},
    { 507, &ppc_fmadddot},   { 508, &ppc_fnmsub},     { 509, &ppc_fnmsubdot},
    { 510, &ppc_fnmadd},     { 511, &ppc_fnmadddot},  { 528, &ppc_fabs},
    { 529, &ppc_fabsdot},    { 536, &ppc_mtfsfidot},  { 558, &ppc_fsel},
    { 559, &ppc_fseldot},    { 562, &ppc_fmult},      { 563, &ppc_fmultdot},
    { 568, &ppc_fmsub},      { 569, &ppc_fmsubdot},   { 570, &ppc_fmadd},
    { 571, &ppc_fmadddot},   { 572, &ppc_fnmsub},     { 573, &ppc_fnmsubdot},
    { 574, &ppc_fnmadd},     { 575, &ppc_fnmadddot},  { 622, &ppc_fsel},
    { 623, &ppc_fseldot},    { 626, &ppc_fmult},      { 627, &ppc_fmultdot},
    { 632, &ppc_fmsub},      { 633, &ppc_fmsubdot},   { 634, &ppc_fmadd},
    { 635, &ppc_fmadddot},   { 636, &ppc_fnmsub},     { 637, &ppc_fnmsubdot},
    { 638, &ppc_fnmadd},     { 639, &ppc_fnmadddot},  { 686, &ppc_fsel},
    { 687, &ppc_fseldot},    { 690, &ppc_fmult},      { 691, &ppc_fmultdot},
    { 696, &ppc_fmsub},      { 697, &ppc_fmsubdot},   { 698, &ppc_fmadd},
    { 699, &ppc_fmadddot},   { 700, &ppc_fnmsub},     { 701, &ppc_fnmsubdot},
    { 702, &ppc_fnmadd},     { 703, &ppc_fnmadddot},  { 750, &ppc_fsel},
    { 751, &ppc_fseldot},    { 754, &ppc_fmult},      { 755, &ppc_fmultdot},
    { 760, &ppc_fmsub},      { 761, &ppc_fmsubdot},   { 762, &ppc_fmadd},
    { 763, &ppc_fmadddot},   { 764, &ppc_fnmsub},     { 765, &ppc_fnmsubdot},
    { 766, &ppc_fnmadd},     { 767, &ppc_fnmadddot},  { 814, &ppc_fsel},
    { 815, &ppc_fseldot},    { 818, &ppc_fmult},      { 819, &ppc_fmultdot},
    { 824, &ppc_fmsub},      { 825, &ppc_fmsubdot},   { 826, &ppc_fmadd},
    { 827, &ppc_fmadddot},   { 828, &ppc_fnmsub},     { 829, &ppc_fnmsubdot},
    { 830, &ppc_fnmadd},     { 831, &ppc_fnmadddot},  { 878, &ppc_fsel},
    { 879, &ppc_fseldot},    { 882, &ppc_fmult},      { 883, &ppc_fmultdot},
    { 888, &ppc_fmsub},      { 889, &ppc_fmsubdot},   { 890, &ppc_fmadd},
    { 891, &ppc_fmadddot},   { 892, &ppc_fnmsub},     { 893, &ppc_fnmsubdot},
    { 894, &ppc_fnmadd},     { 895, &ppc_fnmadddot},  { 942, &ppc_fsel},
    { 943, &ppc_fseldot},    { 946, &ppc_fmult},      { 947, &ppc_fmultdot},
    { 952, &ppc_fmsub},      { 953, &ppc_fmsubdot},   { 954, &ppc_fmadd},
    { 955, &ppc_fmadddot},   { 957, &ppc_fnmsub},     { 958, &ppc_fnmsubdot},
    { 958, &ppc_fnmadd},     { 959, &ppc_fnmadddot},  {1006, &ppc_fsel},
    {1007, &ppc_fseldot},    {1010, &ppc_fmult},      {1011, &ppc_fmultdot},
    {1016, &ppc_fmsub},      {1017, &ppc_fmsubdot},   {1018, &ppc_fmadd},
    {1019, &ppc_fmadddot},   {1020, &ppc_fnmsub},     {1021, &ppc_fnmsubdot},
    {1022, &ppc_fnmadd},     {1023, &ppc_fnmadddot},  {1070, &ppc_fsel},
    {1071, &ppc_fseldot},    {1074, &ppc_fmult},      {1075, &ppc_fmultdot},
    {1080, &ppc_fmsub},      {1081, &ppc_fmsubdot},   {1082, &ppc_fmadd},
    {1083, &ppc_fmadddot},   {1084, &ppc_fnmsub},     {1085, &ppc_fnmsubdot},
    {1086, &ppc_fnmadd},     {1087, &ppc_fnmadddot},  {1134, &ppc_fsel},
    {1135, &ppc_fseldot},    {1138, &ppc_fmult},      {1139, &ppc_fmultdot},
    {1144, &ppc_fmsub},      {1145, &ppc_fmsubdot},   {1146, &ppc_fmadd},
    {1147, &ppc_fmadddot},   {1148, &ppc_fnmsub},     {1149, &ppc_fnmsubdot},
    {1150, &ppc_fnmadd},     {1151, &ppc_fnmadddot},  {1166, &ppc_mffs},
    {1167, &ppc_mffsdot},    {1198, &ppc_fsel},       {1199, &ppc_fseldot},
    {1202, &ppc_fmult},      {1203, &ppc_fmultdot},   {1208, &ppc_fmsub},
    {1209, &ppc_fmsubdot},   {1210, &ppc_fmadd},      {1211, &ppc_fmadddot},
    {1212, &ppc_fnmsub},     {1213, &ppc_fnmsubdot},  {1214, &ppc_fnmadd},
    {1215, &ppc_fnmadddot},  {1262, &ppc_fsel},       {1263, &ppc_fseldot},
    {1266, &ppc_fmult},      {1267, &ppc_fmultdot},   {1272, &ppc_fmsub},
    {1273, &ppc_fmsubdot},   {1274, &ppc_fmadd},      {1275, &ppc_fmadddot},
    {1276, &ppc_fnmsub},     {1277, &ppc_fnmsubdot},  {1278, &ppc_fnmadd},
    {1279, &ppc_fnmadddot},  {1326, &ppc_fsel},       {1327, &ppc_fseldot},
    {1330, &ppc_fmult},      {1331, &ppc_fmultdot},   {1336, &ppc_fmsub},
    {1337, &ppc_fmsubdot},   {1338, &ppc_fmadd},      {1339, &ppc_fmadddot},
    {1340, &ppc_fnmsub},     {1341, &ppc_fnmsubdot},  {1342, &ppc_fnmadd},
    {1343, &ppc_fnmadddot},  {1390, &ppc_fsel},       {1391, &ppc_fseldot},
    {1394, &ppc_fmult},      {1395, &ppc_fmultdot},   {1400, &ppc_fmsub},
    {1401, &ppc_fmsubdot},   {1402, &ppc_fmadd},      {1403, &ppc_fmadddot},
    {1404, &ppc_fnmsub},     {1405, &ppc_fnmsubdot},  {1406, &ppc_fnmadd},
    {1407, &ppc_fnmadddot},  {1422, &ppc_mtfsf},      {1423, &ppc_mtfsfdot},
    {1454, &ppc_fsel},       {1455, &ppc_fseldot},    {1458, &ppc_fmult},
    {1459, &ppc_fmultdot},   {1464, &ppc_fmsub},      {1465, &ppc_fmsubdot},
    {1466, &ppc_fmadd},      {1467, &ppc_fmadddot},   {1468, &ppc_fnmsub},
    {1469, &ppc_fnmsubdot},  {1470, &ppc_fnmadd},     {1471, &ppc_fnmadddot},
    {1518, &ppc_fsel},       {1519, &ppc_fseldot},    {1522, &ppc_fmult},
    {1523, &ppc_fmultdot},   {1528, &ppc_fmsub},      {1529, &ppc_fmsubdot},
    {1530, &ppc_fmadd},      {1531, &ppc_fmadddot},   {1532, &ppc_fnmsub},
    {1533, &ppc_fnmsubdot},  {1534, &ppc_fnmadd},     {1535, &ppc_fnmadddot},
    {1582, &ppc_fsel},       {1583, &ppc_fseldot},    {1586, &ppc_fmult},
    {1587, &ppc_fmultdot},   {1592, &ppc_fmsub},      {1593, &ppc_fmsubdot},
    {1594, &ppc_fmadd},      {1595, &ppc_fmadddot},   {1596, &ppc_fnmsub},
    {1597, &ppc_fnmsubdot},  {1598, &ppc_fnmadd},     {1599, &ppc_fnmadddot},
    {1646, &ppc_fsel},       {1647, &ppc_fseldot},    {1650, &ppc_fmult},
    {1651, &ppc_fmultdot},   {1656, &ppc_fmsub},      {1657, &ppc_fmsubdot},
    {1658, &ppc_fmadd},      {1659, &ppc_fmadddot},   {1660, &ppc_fnmsub},
    {1661, &ppc_fnmsubdot},  {1662, &ppc_fnmadd},     {1663, &ppc_fnmadddot},
    {1710, &ppc_fsel},       {1711, &ppc_fseldot},    {1714, &ppc_fmult},
    {1715, &ppc_fmultdot},   {1720, &ppc_fmsub},      {1721, &ppc_fmsubdot},
    {1722, &ppc_fmadd},      {1723, &ppc_fmadddot},   {1724, &ppc_fnmsub},
    {1725, &ppc_fnmsubdot},  {1726, &ppc_fnmadd},     {1727, &ppc_fnmadddot},
    {1774, &ppc_fsel},       {1775, &ppc_fseldot},    {1778, &ppc_fmult},
    {1779, &ppc_fmultdot},   {1784, &ppc_fmsub},      {1785, &ppc_fmsubdot},
    {1786, &ppc_fmadd},      {1787, &ppc_fmadddot},   {1788, &ppc_fnmsub},
    {1789, &ppc_fnmsubdot},  {1790, &ppc_fnmadd},     {1791, &ppc_fnmadddot},
    {1838, &ppc_fsel},       {1839, &ppc_fseldot},    {1842, &ppc_fmult},
    {1843, &ppc_fmultdot},   {1848, &ppc_fmsub},      {1849, &ppc_fmsubdot},
    {1850, &ppc_fmadd},      {1851, &ppc_fmadddot},   {1852, &ppc_fnmsub},
    {1853, &ppc_fnmsubdot},  {1854, &ppc_fnmadd},     {1855, &ppc_fnmadddot},
    {1902, &ppc_fsel},       {1903, &ppc_fseldot},    {1906, &ppc_fmult},
    {1907, &ppc_fmultdot},   {1912, &ppc_fmsub},      {1913, &ppc_fmsubdot},
    {1914, &ppc_fmadd},      {1915, &ppc_fmadddot},   {1916, &ppc_fnmsub},
    {1917, &ppc_fnmsubdot},  {1918, &ppc_fnmadd},     {1919, &ppc_fnmadddot},
    {1966, &ppc_fsel},       {1967, &ppc_fseldot},    {1970, &ppc_fmult},
    {1971, &ppc_fmultdot},   {1976, &ppc_fmsub},      {1977, &ppc_fmsubdot},
    {1978, &ppc_fmadd},      {1979, &ppc_fmadddot},   {1980, &ppc_fnmsub},
    {1981, &ppc_fnmsubdot},  {1982, &ppc_fnmadd},     {1983, &ppc_fnmadddot},
    {2030, &ppc_fsel},       {2031, &ppc_fseldot},    {2034, &ppc_fmult},
    {2035, &ppc_fmultdot},   {2040, &ppc_fmsub},      {2041, &ppc_fmsubdot},
    {2042, &ppc_fmadd},      {2043, &ppc_fmadddot},   {2044, &ppc_fnmsub},
    {2045, &ppc_fnmsubdot},  {2046, &ppc_fnmadd},     {2047, &ppc_fnmadddot}
};

/** Opcode decoding functions. */

void ppc_illegalop() {
    uint8_t illegal_code = ppc_cur_instruction >> 26;
    uint32_t grab_it = (uint32_t)illegal_code;
    LOG_F(ERROR, "Illegal opcode reported: %d Report this! \n", grab_it);
    exit(-1);
}

void ppc_illegalsubop19() {
    uint16_t illegal_subcode = ppc_cur_instruction & 2047;
    uint32_t grab_it = (uint32_t)illegal_subcode;
    LOG_F(ERROR, "Illegal subopcode for 19 reported: %d Report this! \n", grab_it);
    exit(-1);
}

void ppc_illegalsubop31() {
    uint16_t illegal_subcode = ppc_cur_instruction & 2047;
    uint32_t grab_it = (uint32_t)illegal_subcode;
    LOG_F(ERROR, "Illegal subopcode for 31 reported: %d Report this! \n", grab_it);
    exit(-1);
}

void ppc_illegalsubop59() {
    uint16_t illegal_subcode = ppc_cur_instruction & 2047;
    uint32_t grab_it = (uint32_t)illegal_subcode;
    LOG_F(ERROR, "Illegal subopcode for 59 reported: %d Report this! \n", grab_it);
    exit(-1);
}

void ppc_illegalsubop63() {
    uint16_t illegal_subcode = ppc_cur_instruction & 2047;
    uint32_t grab_it = (uint32_t)illegal_subcode;
    LOG_F(ERROR, "Illegal subopcode for 63 reported: %d Report this! \n", grab_it);
    exit(-1);
}


void ppc_opcode4() {
    LOG_F(INFO, "Reading from Opcode 4 table \n");
    uint8_t subop_grab = ppc_cur_instruction & 3;
    uint32_t regrab = (uint32_t)subop_grab;
    LOG_F(ERROR, "Executing subopcode entry %d \n"
           ".. or would if I bothered to implement it. SORRY!", regrab);
    exit(0);
}

void ppc_opcode16() {
    SubOpcode16Grabber[ppc_cur_instruction & 3]();
}

void ppc_opcode18() {
    SubOpcode18Grabber[ppc_cur_instruction & 3]();
}

void ppc_opcode19() {
    uint16_t subop_grab = ppc_cur_instruction & 2047;

#ifdef EXHAUSTIVE_DEBUG
    uint32_t regrab = (uint32_t)subop_grab;
    LOG_F(INFO, "Executing Opcode 19 table subopcode entry \n", regrab);
#endif // EXHAUSTIVE_DEBUG

    if (subop_grab == 32) {
        ppc_bclr();
    }
    else if (subop_grab == 33) {
        ppc_bclrl();
}
    else if (subop_grab == 1056) {
        ppc_bcctr();
    }
    else if (subop_grab == 1057) {
        ppc_bcctrl();

    }
    else {
        switch (subop_grab) {
        case 66:
            ppc_crnor();
            break;
        case 100:
            ppc_rfi();
            break;
        case 258:
            ppc_crandc();
            break;
        case 300:
            ppc_isync();
            break;
        case 386:
            ppc_crxor();
            break;
        case 450:
            ppc_crnand();
            break;
        case 514:
            ppc_crand();
            break;
        case 578:
            ppc_creqv();
            break;
        case 834:
            ppc_crorc();
            break;
        case 898:
            ppc_cror();
            break;
        default: //Illegal opcode - should never happen
            ppc_illegalsubop19();
        }
    }
}

void ppc_opcode31() {
    uint16_t subop_grab = (ppc_cur_instruction & 2047) >> 1;

    rc_flag = ppc_cur_instruction & 0x1;
    oe_flag = ppc_cur_instruction & 0x400;

#ifdef EXHAUSTIVE_DEBUG
    uint32_t regrab = (uint32_t)subop_grab;
    LOG_F(INFO, "Executing Opcode 63 table subopcode entry \n", regrab);
#endif // EXHAUSTIVE_DEBUG

    SubOpcode31Grabber[subop_grab]();
}

void ppc_opcode59() {
    uint16_t subop_grab = ppc_cur_instruction & 2047;
    rc_flag = subop_grab & 1;
#ifdef EXHAUSTIVE_DEBUG
    uint32_t regrab = (uint32_t)subop_grab;
    LOG_F(INFO, "Executing Opcode 59 table subopcode entry \n", regrab);
#endif // EXHAUSTIVE_DEBUG
    SubOpcode59Grabber[subop_grab]();
}

void ppc_opcode63() {
    uint16_t subop_grab = ppc_cur_instruction & 2047;
    rc_flag = subop_grab & 1;
#ifdef EXHAUSTIVE_DEBUG
    uint32_t regrab = (uint32_t)subop_grab;
    LOG_F(INFO, "Executing Opcode 63 table subopcode entry \n", regrab);
#endif // EXHAUSTIVE_DEBUG
    SubOpcode63Grabber[subop_grab]();
}

void ppc_main_opcode() {
    //Grab the main opcode
    uint8_t ppc_mainop = (ppc_cur_instruction >> 26) & 63;
    OpcodeGrabber[ppc_mainop]();
}

/** Old time base register (TBR) update code. */
void tbr_update()
{
    clock_t clock_test_current = clock();
    uint32_t test_clock = ((uint32_t)(clock_test_current - clock_test_begin)) / CLOCKS_PER_SEC;
    if (test_clock) {
        if (ppc_state.tbr[0] != 0xFFFFFFFF) {
            ppc_state.tbr[0]++;
        }
        else {
            ppc_state.tbr[0] = 0;
            if (ppc_state.tbr[1] != 0xFFFFFFFF) {
                ppc_state.tbr[1]++;
            }
            else {
                ppc_state.tbr[1] = 0;
            }
        }
        clock_test_begin = clock();
        //Placeholder Decrementing Code
        if (ppc_state.spr[22] > 0) {
            ppc_state.spr[22]--;
        }
    }
}

/** Execute PPC code as long as power is on. */
#if 0
void ppc_exec()
{
    while (power_on) {
        //printf("PowerPC Address: %x \n", ppc_state.pc);
        quickinstruction_translate(ppc_state.pc);
        ppc_main_opcode();
        if (grab_branch & !grab_exception) {
            ppc_state.pc = ppc_next_instruction_address;
            grab_branch = 0;
            tbr_update();
        }
        else if (grab_return | grab_exception) {
            ppc_state.pc = ppc_next_instruction_address;
            grab_exception = 0;
            grab_return = 0;
            tbr_update();
        }
        else {
            ppc_state.pc += 4;
            tbr_update();
        }
    }
}
#else
void ppc_exec()
{
    uint32_t bb_start_la, page_start;
    uint8_t* pc_real;

    /* start new basic block */
    bb_start_la = ppc_state.pc;
    bb_kind = BB_end_kind::BB_NONE;

    if (setjmp(exc_env)) {
        /* reaching here means we got a low-level exception */
        timebase_counter += (ppc_state.pc - bb_start_la) >> 2;
        bb_start_la = ppc_next_instruction_address;
        pc_real = quickinstruction_translate(bb_start_la);
        page_start = bb_start_la & 0xFFFFF000;
        ppc_state.pc = bb_start_la;
        bb_kind = BB_end_kind::BB_NONE;
        goto again;
    }

    /* initial MMU translation for the current code page. */
    pc_real = quickinstruction_translate(bb_start_la);

    /* set current code page limits */
    page_start = bb_start_la & 0xFFFFF000;

again:
    while (power_on) {
        ppc_main_opcode();
        if (bb_kind != BB_end_kind::BB_NONE) {
            timebase_counter += (ppc_state.pc - bb_start_la) >> 2;
            bb_start_la = ppc_next_instruction_address;
            if ((ppc_next_instruction_address & 0xFFFFF000) != page_start) {
                page_start = bb_start_la & 0xFFFFF000;
                pc_real = quickinstruction_translate(bb_start_la);
            }
            else {
                pc_real += (int)bb_start_la - (int)ppc_state.pc;
                ppc_set_cur_instruction(pc_real);
            }
            ppc_state.pc = bb_start_la;
            bb_kind = BB_end_kind::BB_NONE;
        }
        else {
            ppc_state.pc += 4;
            pc_real += 4;
            ppc_set_cur_instruction(pc_real);
        }
    }
}
#endif

/** Execute one PPC instruction. */
#if 0
void ppc_exec_single()
{
    quickinstruction_translate(ppc_state.pc);
    ppc_main_opcode();
    if (grab_branch && !grab_exception) {
        ppc_state.pc = ppc_next_instruction_address;
        grab_branch = 0;
        tbr_update();
    }
    else if (grab_return || grab_exception) {
        ppc_state.pc = ppc_next_instruction_address;
        grab_exception = 0;
        grab_return = 0;
        tbr_update();
    }
    else {
        ppc_state.pc += 4;
        tbr_update();
    }
}
#else
void ppc_exec_single()
{
    if (setjmp(exc_env)) {
        /* reaching here means we got a low-level exception */
        timebase_counter += 1;
        ppc_state.pc = ppc_next_instruction_address;
        bb_kind = BB_end_kind::BB_NONE;
        return;
    }

    quickinstruction_translate(ppc_state.pc);
    ppc_main_opcode();
    if (bb_kind != BB_end_kind::BB_NONE) {
        ppc_state.pc = ppc_next_instruction_address;
        bb_kind = BB_end_kind::BB_NONE;
    }
    else {
        ppc_state.pc += 4;
    }
    timebase_counter += 1;
}
#endif

/** Execute PPC code until goal_addr is reached. */
#if 0
void ppc_exec_until(uint32_t goal_addr)
{
    while (ppc_state.pc != goal_addr) {
        quickinstruction_translate(ppc_state.pc);
        ppc_main_opcode();
        if (grab_branch && !grab_exception) {
            ppc_state.pc = ppc_next_instruction_address;
            grab_branch = 0;
            tbr_update();
        }
        else if (grab_return || grab_exception) {
            ppc_state.pc = ppc_next_instruction_address;
            grab_exception = 0;
            grab_return = 0;
            tbr_update();
        }
        else {
            ppc_state.pc += 4;
            tbr_update();
        }
        ppc_cur_instruction = 0;
    }
}
#else
void ppc_exec_until(uint32_t goal_addr)
{
    uint32_t bb_start_la, page_start;
    uint8_t* pc_real;

    /* start new basic block */
    bb_start_la = ppc_state.pc;
    bb_kind = BB_end_kind::BB_NONE;

    if (setjmp(exc_env)) {
        /* reaching here means we got a low-level exception */
        timebase_counter += (ppc_state.pc - bb_start_la) >> 2;
        bb_start_la = ppc_next_instruction_address;
        pc_real = quickinstruction_translate(bb_start_la);
        page_start = bb_start_la & 0xFFFFF000;
        ppc_state.pc = bb_start_la;
        bb_kind = BB_end_kind::BB_NONE;
        goto again;
    }

    /* initial MMU translation for the current code page. */
    pc_real = quickinstruction_translate(bb_start_la);

    /* set current code page limits */
    page_start = bb_start_la & 0xFFFFF000;

again:
    while (ppc_state.pc != goal_addr) {
        ppc_main_opcode();
        if (bb_kind != BB_end_kind::BB_NONE) {
            timebase_counter += (ppc_state.pc - bb_start_la) >> 2;
            bb_start_la = ppc_next_instruction_address;
            if ((ppc_next_instruction_address & 0xFFFFF000) != page_start) {
                page_start = bb_start_la & 0xFFFFF000;
                pc_real = quickinstruction_translate(bb_start_la);
            }
            else {
                pc_real += (int)bb_start_la - (int)ppc_state.pc;
                ppc_set_cur_instruction(pc_real);
            }
            ppc_state.pc = bb_start_la;
            bb_kind = BB_end_kind::BB_NONE;
        }
        else {
            ppc_state.pc += 4;
            pc_real += 4;
            ppc_set_cur_instruction(pc_real);
        }
    }
}
#endif

void ppc_opcode31_init() {
    SubOpcode31Grabber[0] = ppc_cmp;
    SubOpcode31Grabber[4] = ppc_tw;
    SubOpcode31Grabber[32] = ppc_cmpl;

    SubOpcode31Grabber[8] = SubOpcode31Grabber[520] = ppc_subfc;
    SubOpcode31Grabber[40] = SubOpcode31Grabber[552] = ppc_subf;
    SubOpcode31Grabber[104] = SubOpcode31Grabber[616] = ppc_neg;
    SubOpcode31Grabber[136] = SubOpcode31Grabber[648] = ppc_subfe;
    SubOpcode31Grabber[200] = SubOpcode31Grabber[712] = ppc_subfze;
    SubOpcode31Grabber[232] = SubOpcode31Grabber[744] = ppc_subfme;

    SubOpcode31Grabber[10] = SubOpcode31Grabber[522] = ppc_addc;
    SubOpcode31Grabber[138] = SubOpcode31Grabber[650] = ppc_adde;
    SubOpcode31Grabber[202] = SubOpcode31Grabber[714] = ppc_addze;
    SubOpcode31Grabber[234] = SubOpcode31Grabber[746] = ppc_addme;
    SubOpcode31Grabber[266] = SubOpcode31Grabber[778] = ppc_add;

    SubOpcode31Grabber[11] = ppc_mulhwu;
    SubOpcode31Grabber[75] = ppc_mulhw;
    SubOpcode31Grabber[235] = SubOpcode31Grabber[747] = ppc_mullw;
    SubOpcode31Grabber[459] = SubOpcode31Grabber[971] = ppc_divwu;
    SubOpcode31Grabber[491] = SubOpcode31Grabber[1003] = ppc_divw;

    SubOpcode31Grabber[20] = ppc_lwarx;
    SubOpcode31Grabber[23] = ppc_lwzx;
    SubOpcode31Grabber[55] = ppc_lwzux;
    SubOpcode31Grabber[87] = ppc_lbzx;
    SubOpcode31Grabber[119] = ppc_lbzux;
    SubOpcode31Grabber[279] = ppc_lhzx;
    SubOpcode31Grabber[311] = ppc_lhzux;
    SubOpcode31Grabber[343] = ppc_lhax;
    SubOpcode31Grabber[375] = ppc_lhaux;
    SubOpcode31Grabber[533] = ppc_lswx;
    SubOpcode31Grabber[534] = ppc_lwbrx;
    SubOpcode31Grabber[535] = ppc_lfsx;
    SubOpcode31Grabber[567] = ppc_lfsux;
    SubOpcode31Grabber[597] = ppc_lswi;
    SubOpcode31Grabber[599] = ppc_lfdx;
    SubOpcode31Grabber[631] = ppc_lfdux;
    SubOpcode31Grabber[790] = ppc_lhbrx;

    SubOpcode31Grabber[150] = ppc_stwcx;
    SubOpcode31Grabber[151] = ppc_stwx;
    SubOpcode31Grabber[183] = ppc_stwux;
    SubOpcode31Grabber[215] = ppc_stbx;
    SubOpcode31Grabber[247] = ppc_stbux;
    SubOpcode31Grabber[407] = ppc_sthx;
    SubOpcode31Grabber[439] = ppc_sthux;
    SubOpcode31Grabber[661] = ppc_stswx;
    SubOpcode31Grabber[662] = ppc_stwbrx;
    SubOpcode31Grabber[663] = ppc_stfsx;
    SubOpcode31Grabber[695] = ppc_stfsux;
    SubOpcode31Grabber[725] = ppc_stswi;
    SubOpcode31Grabber[727] = ppc_stfdx;
    SubOpcode31Grabber[759] = ppc_stfdux;
    SubOpcode31Grabber[918] = ppc_sthbrx;
    SubOpcode31Grabber[983] = ppc_stfiwx;

    SubOpcode31Grabber[24] = ppc_slw;
    SubOpcode31Grabber[28] = ppc_and;
    SubOpcode31Grabber[60] = ppc_andc;
    SubOpcode31Grabber[124] = ppc_nor;
    SubOpcode31Grabber[284] = ppc_eqv;
    SubOpcode31Grabber[316] = ppc_xor;
    SubOpcode31Grabber[412] = ppc_orc;
    SubOpcode31Grabber[444] = ppc_or;
    SubOpcode31Grabber[476] = ppc_nand;
    SubOpcode31Grabber[536] = ppc_srw;
    SubOpcode31Grabber[792] = ppc_sraw;
    SubOpcode31Grabber[824] = ppc_srawi;
    SubOpcode31Grabber[922] = ppc_extsh;
    SubOpcode31Grabber[954] = ppc_extsb;

    SubOpcode31Grabber[26] = ppc_cntlzw;

    SubOpcode31Grabber[19] = ppc_mfcr;
    SubOpcode31Grabber[83] = ppc_mfmsr;
    SubOpcode31Grabber[144] = ppc_mtcrf;
    SubOpcode31Grabber[146] = ppc_mtmsr;
    SubOpcode31Grabber[210] = ppc_mtsr;
    SubOpcode31Grabber[242] = ppc_mtsrin;
    SubOpcode31Grabber[256] = ppc_mcrxr;
    SubOpcode31Grabber[339] = ppc_mfspr;
    SubOpcode31Grabber[371] = ppc_mftb;
    SubOpcode31Grabber[467] = ppc_mtspr;
    SubOpcode31Grabber[595] = ppc_mfsr;
    SubOpcode31Grabber[659] = ppc_mfsrin;

    SubOpcode31Grabber[54] = ppc_dcbst;
    SubOpcode31Grabber[86] = ppc_dcbf;
    SubOpcode31Grabber[246] = ppc_dcbtst;
    SubOpcode31Grabber[278] = ppc_dcbt;
    SubOpcode31Grabber[598] = ppc_sync;
    SubOpcode31Grabber[470] = ppc_dcbi;
    SubOpcode31Grabber[1014] = ppc_dcbz;

    SubOpcode31Grabber[29] = power_maskg;
    SubOpcode31Grabber[107] = SubOpcode31Grabber[619] = power_mul;
    SubOpcode31Grabber[152] = power_slq;
    SubOpcode31Grabber[153] = power_sle;
    SubOpcode31Grabber[184] = power_sliq;
    SubOpcode31Grabber[216] = power_sllq;
    SubOpcode31Grabber[217] = power_sleq;
    SubOpcode31Grabber[248] = power_slliq;
    SubOpcode31Grabber[264] = SubOpcode31Grabber[776] = power_doz;
    SubOpcode31Grabber[277] = power_lscbx;
    SubOpcode31Grabber[331] = SubOpcode31Grabber[843] = power_div;
    SubOpcode31Grabber[360] = SubOpcode31Grabber[872] = power_abs;
    SubOpcode31Grabber[363] = SubOpcode31Grabber[875] = power_divs;
    SubOpcode31Grabber[488] = SubOpcode31Grabber[1000] = power_nabs;
    SubOpcode31Grabber[531] = power_clcs;
    SubOpcode31Grabber[537] = power_rrib;
    SubOpcode31Grabber[541] = power_maskir;
    SubOpcode31Grabber[664] = power_srq;
    SubOpcode31Grabber[665] = power_sre;
    SubOpcode31Grabber[696] = power_sriq;
    SubOpcode31Grabber[728] = power_srlq;
    SubOpcode31Grabber[729] = power_sreq;
    SubOpcode31Grabber[760] = power_srliq;
    SubOpcode31Grabber[920] = power_sraq;
    SubOpcode31Grabber[921] = power_srea;
    SubOpcode31Grabber[952] = power_sraiq;

    SubOpcode31Grabber[306] = ppc_tlbie;
    SubOpcode31Grabber[370] = ppc_tlbia;
    SubOpcode31Grabber[566] = ppc_tlbsync;
    SubOpcode31Grabber[854] = ppc_eieio;
    SubOpcode31Grabber[982] = ppc_icbi;
    SubOpcode31Grabber[978] = ppc_tlbld;
    SubOpcode31Grabber[1010] = ppc_tlbli;
}

void ppc_init_opcode_tables() {
    ppc_opcode31_init();

    //TODO: INSERT TABLES FOR OPCODES 59 and 63
}

void ppc_cpu_init(MemCtrlBase *mem_ctrl, uint32_t proc_version)
{
    int i;

    mem_ctrl_instance = mem_ctrl;

    clock_test_begin = clock();
    timebase_counter = 0;

    /* zero all GPRs as prescribed for MPC601 */
    /* For later PPC CPUs, GPR content is undefined */
    for (i = 0; i < 32; i++) {
        ppc_state.gpr[i] = 0;
    }

    /* zero all FPRs as prescribed for MPC601 */
    /* For later PPC CPUs, GPR content is undefined */
    for (i = 0; i < 32; i++) {
        ppc_state.fpr[i].int64_r = 0;
    }

    /* zero all segment registers as prescribed for MPC601 */
    /* For later PPC CPUs, SR content is undefined */
    for (i = 0; i < 16; i++) {
        ppc_state.sr[i] = 0;
    }

    ppc_state.cr = 0;
    ppc_state.fpscr = 0;

    ppc_state.pc = 0;

    ppc_state.tbr[0] = 0;
    ppc_state.tbr[1] = 0;

    /* zero all SPRs */
    for (i = 0; i < 1024; i++) {
        ppc_state.spr[i] = 0;
    }

    ppc_state.spr[SPR::PVR] = proc_version;

    if ((proc_version & 0xFFFF0000) == 0x00010000) {
        /* MPC601 sets MSR[ME] bit during hard reset / Power-On */
        ppc_state.msr = 0x1040;
    } else {
        ppc_state.msr = 0x40;
        ppc_state.spr[SPR::DEC] = 0xFFFFFFFFUL;
    }

    ppc_mmu_init();

    ppc_init_opcode_tables();

    /* redirect code execution to reset vector */
    ppc_state.pc = 0xFFF00100;
}

void print_gprs()
{
    for (int i = 0; i < 32; i++)
        cout << "GPR " << dec << i << " : " << uppercase << hex
            << ppc_state.gpr[i] << endl;

    cout << "PC: " << uppercase << hex << ppc_state.pc << endl;
    cout << "LR: " << uppercase << hex << ppc_state.spr[SPR::LR] << endl;
    cout << "CR: " << uppercase << hex << ppc_state.cr << endl;
    cout << "CTR: " << uppercase << hex << ppc_state.spr[SPR::CTR] << endl;
    cout << "XER: " << uppercase << hex << ppc_state.spr[SPR::XER] << endl;
    cout << "MSR: " << uppercase << hex << ppc_state.msr << endl;
}

void print_fprs()
{
    for (int i = 0; i < 32; i++)
        cout << "FPR " << dec << i << " : " << ppc_state.fpr[i].dbl64_r << endl;
}

static map<string, int> SPRName2Num = {
    {"XER", SPR::XER}, {"LR", SPR::LR}, {"CTR", SPR::CTR}, {"DEC", SPR::DEC},
    {"PVR", SPR::PVR}
};

uint64_t reg_op(string &reg_name, uint64_t val, bool is_write)
{
    string reg_name_u, reg_num_str;
    unsigned reg_num;
    map<string, int>::iterator spr;

    if (reg_name.length() < 2)
        goto bail_out;

    reg_name_u = reg_name;

    /* convert reg_name string to uppercase */
    std::for_each(reg_name_u.begin(), reg_name_u.end(), [](char & c) {
        c = ::toupper(c);
    });

    try {
        if (reg_name_u == "PC") {
            if (is_write)
                ppc_state.pc = val;
            return ppc_state.pc;
        }
        if (reg_name_u == "MSR") {
            if (is_write)
                ppc_state.msr = val;
            return ppc_state.msr;
        }
        if (reg_name_u == "CR") {
            if (is_write)
                ppc_state.cr = val;
            return ppc_state.cr;
        }
        if (reg_name_u == "FPSCR") {
            if (is_write)
                ppc_state.fpscr = val;
            return ppc_state.fpscr;
        }

        if (reg_name_u.substr(0, 1) == "R") {
            reg_num_str = reg_name_u.substr(1);
            reg_num = stoul(reg_num_str, NULL, 0);
            if (reg_num < 32) {
                if (is_write)
                    ppc_state.gpr[reg_num] = val;
                return ppc_state.gpr[reg_num];
            }
        }

        if (reg_name_u.substr(0, 1) == "FR") {
            reg_num_str = reg_name_u.substr(2);
            reg_num = stoul(reg_num_str, NULL, 0);
            if (reg_num < 32) {
                if (is_write)
                    ppc_state.fpr[reg_num].int64_r = val;
                return ppc_state.fpr[reg_num].int64_r;
            }
        }

        if (reg_name_u.substr(0, 3) == "SPR") {
            reg_num_str = reg_name_u.substr(3);
            reg_num = stoul(reg_num_str, NULL, 0);
            if (reg_num < 1024) {
                if (is_write)
                    ppc_state.spr[reg_num] = val;
                return ppc_state.spr[reg_num];
            }
        }

        if (reg_name_u.substr(0, 2) == "SR") {
            reg_num_str = reg_name_u.substr(2);
            reg_num = stoul(reg_num_str, NULL, 0);
            if (reg_num < 16) {
                if (is_write)
                    ppc_state.sr[reg_num] = val;
                return ppc_state.sr[reg_num];
            }
        }

        spr = SPRName2Num.find(reg_name_u);
        if (spr != SPRName2Num.end()) {
            if (is_write)
                ppc_state.spr[spr->second] = val;
            return ppc_state.spr[spr->second];
        }
    }
    catch (...) {
    }

bail_out:
    throw std::invalid_argument(string("Unknown register ") + reg_name);
}

uint64_t get_reg(string &reg_name)
{
    return reg_op(reg_name, 0, false);
}

void set_reg(string &reg_name, uint64_t val)
{
    reg_op(reg_name, val, true);
}
