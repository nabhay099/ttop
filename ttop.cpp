// ╔══════════════════════════════════════════════════════════════════╗
// ║  ttop v4.0.2 — Terminal System Monitor                           ║
// ║  Grid CPU • iGPU/Nvidia • HW/Proc Split • Custom BG #020302      ║
// ║  Compile: g++ -std=c++17 -O2 -o ttop ttop.cpp -lncursesw        ║
// ╚══════════════════════════════════════════════════════════════════╝

#include <ncurses.h>
#include <locale.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <ctime>
#include <cstring>
#include <dirent.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <signal.h>
#include <chrono>
#include <sys/wait.h>
#include <functional>
#include <thread>
#include <atomic>
#include <random>
#include <numeric>
#include <mutex>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────
//  COLOR PAIRS & CUSTOM BACKGROUND
// ─────────────────────────────────────────────────────────────────────
#define C_BG        1
#define C_TEXT      2
#define C_DIM       3
#define C_BORDER    4
#define C_USAGE_1   5  // Purple
#define C_USAGE_2   6  // Yellow
#define C_WATT_1    7  // Yellow
#define C_WATT_2    8  // Blue
#define C_RED       9
#define C_GREEN     10
#define C_CYAN      11
#define C_SELECTED  12
#define C_CUSTOM_BG 20 

static void applyColors() {
    int bg = COLOR_BLACK;
    if (can_change_color()) {
        init_color(C_CUSTOM_BG, 8, 12, 8); // #020302
        bg = C_CUSTOM_BG;
    }
    init_pair(C_BG, COLOR_WHITE, bg);
    init_pair(C_TEXT, COLOR_WHITE, bg);
    init_pair(C_DIM, COLOR_WHITE, bg);
    init_pair(C_BORDER, COLOR_CYAN, bg);
    init_pair(C_USAGE_1, COLOR_MAGENTA, bg);
    init_pair(C_USAGE_2, COLOR_YELLOW, bg);
    init_pair(C_WATT_1, COLOR_YELLOW, bg);
    init_pair(C_WATT_2, COLOR_BLUE, bg);
    init_pair(C_RED, COLOR_RED, bg);
    init_pair(C_GREEN, COLOR_GREEN, bg);
    init_pair(C_CYAN, COLOR_CYAN, bg);
    init_pair(C_SELECTED, COLOR_BLACK, COLOR_WHITE);
}

// ─────────────────────────────────────────────────────────────────────
//  GLOBALS
// ─────────────────────────────────────────────────────────────────────
static volatile bool g_resize = false;
static volatile bool g_quit   = false;

static struct {
    bool cpu=true, mem=true, net=true, disk=true, gpu=true, proc=true, hw=true;
    int  update_ms=500;
    int  proc_scroll=0;
    int  proc_selected=0;
    bool signal_mode=false;
} g_cfg;

struct FPS {
    int frames=0; double fps=0;
    std::chrono::steady_clock::time_point t0=std::chrono::steady_clock::now();
    void tick(){
        frames++;
        auto now=std::chrono::steady_clock::now();
        double el=std::chrono::duration<double>(now-t0).count();
        if(el>=1.0){fps=frames/el;frames=0;t0=now;}
    }
} g_fps;

// ─────────────────────────────────────────────────────────────────────
//  DATA STRUCTURES
// ─────────────────────────────────────────────────────────────────────
struct CPUCore {
    long long user=0,nice=0,system=0,idle=0,iowait=0,irq=0,softirq=0,steal=0;
    long long total() const {return user+nice+system+idle+iowait+irq+softirq+steal;}
};
struct CPUStats { 
    std::vector<CPUCore> cores; 
    double freq_mhz=0; 
    std::string governor="unknown"; 
    double package_w=0.0;
};
struct MemStats {
    long total=0,free=0,available=0,buffers=0,cached=0,sreclaimable=0;
    long swap_total=0,swap_free=0;
    long used()      const {return total-available;}
    long swap_used() const {return swap_total-swap_free;}
};
struct Sensor   { std::string label,type; float temp=0; };
struct NetIface { std::string name; long long rx_bytes=0,tx_bytes=0; double rx_rate=0,tx_rate=0; bool primary=false; };
struct DiskInfo { std::string mount,device; unsigned long long total=0,used=0; double read_rate=0,write_rate=0; };
struct DiskIO   { long long rb=0,wb=0; };
struct ProcInfo { int pid=0; std::string name; char state=' '; long mem_kb=0; long long cpu_ticks=0; double cpu_pct=0; std::string user; };

struct GPUStats { 
    std::string name="No GPU Detected"; 
    int usage_pct=0; 
    double power_w=0.0; 
    double power_max_w=0.0; 
    double temp_c=0.0; 
    bool detected=false; 
};

struct HWInfo {
    std::string model,cpu,gpu,kernel,net_iface,mac;
    int cores=0,threads=0; long ram_gb=0; bool loaded=false;
};

struct SysMeta { std::string hostname,cpu_model; int cores=0; double uptime=0; };

struct AppState {
    SysMeta meta; HWInfo hw; GPUStats gpu;
    CPUStats cpu_prev,cpu_curr; std::vector<double> cpu_pct; std::deque<double> cpu_hist;
    std::deque<double> gpu_hist;
    std::vector<Sensor> sensors; MemStats mem;
    std::vector<NetIface> net_prev,net_curr; std::map<std::string,std::deque<double>> rx_hist,tx_hist;
    std::vector<DiskInfo> disks; std::map<std::string,DiskIO> disk_io_prev,disk_io_curr;
    std::vector<ProcInfo> procs;
};

// ─────────────────────────────────────────────────────────────────────
//  UTILITY
// ─────────────────────────────────────────────────────────────────────
static std::string trim(const std::string& s){
    auto a=s.find_first_not_of(" \t\r\n");
    if(a==std::string::npos) return "";
    return s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
static std::string fmtBytes(unsigned long long b){
    std::ostringstream o;
    if(b<1024) o<<b<<"B";
    else if(b<1048576) o<<std::fixed<<std::setprecision(1)<<b/1024.0<<"K";
    else if(b<1073741824) o<<std::fixed<<std::setprecision(1)<<b/1048576.0<<"M";
    else o<<std::fixed<<std::setprecision(2)<<b/1073741824.0<<"G";
    return o.str();
}
static std::string fmtRate(double bps){
    if(bps<0) bps=0;
    std::ostringstream o;
    if(bps<1024) o<<(int)bps<<"B/s";
    else if(bps<1048576) o<<std::fixed<<std::setprecision(1)<<bps/1024<<"K/s";
    else o<<std::fixed<<std::setprecision(2)<<bps/1048576<<"M/s";
    return o.str();
}
static std::string padR(std::string s,int w){ if((int)s.size()<w) s.append(w-s.size(),' '); else if((int)s.size()>w) s=s.substr(0,w); return s; }
static int usageColor(double p){ return (p<60) ? C_USAGE_1 : C_USAGE_2; }
static int tempColor(float t){ if(t<55) return C_GREEN; if(t<75) return C_USAGE_2; return C_RED; }

// ─────────────────────────────────────────────────────────────────────
//  GOVERNOR CONTROL
// ─────────────────────────────────────────────────────────────────────
static void cycleGovernor() {
    std::string path = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor";
    std::ifstream f(path);
    std::string current;
    if(f) std::getline(f, current);
    current = trim(current);
    
    std::vector<std::string> govs = {"performance", "powersave", "schedutil", "ondemand", "conservative"};
    std::string next = "performance";
    for(size_t i=0; i<govs.size(); i++) {
        if(govs[i] == current) {
            next = govs[(i+1) % govs.size()];
            break;
        }
    }
    for(int i=0; i<64; i++) {
        std::string p = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_governor";
        std::ofstream out(p);
        if(out.is_open()) out << next; else break;
    }
}

// ─────────────────────────────────────────────────────────────────────
//  READERS (Robust GPU & CPU)
// ─────────────────────────────────────────────────────────────────────
static long long prev_rapl_uj = 0;
static std::chrono::steady_clock::time_point prev_rapl_time = std::chrono::steady_clock::now();

static GPUStats readGPU(){
    GPUStats g;
    // 1. Try Nvidia
    FILE* pipe = popen("nvidia-smi --query-gpu=name,utilization.gpu,power.draw,power.limit,temperature.gpu --format=csv,noheader,nounits 2>/dev/null", "r");
    if(pipe){
        char buf[256];
        if(fgets(buf, sizeof(buf), pipe)){
            std::stringstream ss(buf);
            std::string token;
            if(std::getline(ss, token, ',')) g.name = trim(token);
            if(std::getline(ss, token, ',')) { try { g.usage_pct = std::stoi(trim(token)); } catch(...) {} }
            if(std::getline(ss, token, ',')) { try { g.power_w = std::stod(trim(token)); } catch(...) {} }
            if(std::getline(ss, token, ',')) { try { g.power_max_w = std::stod(trim(token)); } catch(...) {} }
            if(std::getline(ss, token, ',')) { try { g.temp_c = std::stod(trim(token)); } catch(...) {} }
            g.detected = true;
        }
        pclose(pipe);
    }
    
    // 2. Fallback: AMD / Intel iGPU (sysfs)
    if(!g.detected){
        for(int i=0; i<4; i++){
            std::string path = "/sys/class/drm/card" + std::to_string(i) + "/device/gpu_busy_percent";
            std::ifstream f(path);
            if(f){
                f >> g.usage_pct;
                g.detected = true;
                g.name = "iGPU (card" + std::to_string(i) + ")";
                break;
            }
        }
        for(int i=0; i<10; i++){
            std::string base = "/sys/class/hwmon/hwmon" + std::to_string(i) + "/";
            std::ifstream nf(base + "name");
            if(!nf) continue;
            std::string chip; nf >> chip;
            if(chip.find("amdgpu")!=std::string::npos || chip.find("i915")!=std::string::npos || chip.find("xe")!=std::string::npos){
                std::ifstream tf(base + "temp1_input");
                if(tf){ int mc; tf >> mc; g.temp_c = mc / 1000.0; }
                
                long long uw = 0;
                std::ifstream pf_avg(base + "power1_average");
                if(pf_avg) { pf_avg >> uw; g.power_w = uw / 1000000.0; }
                else {
                    std::ifstream pf_in(base + "power1_input");
                    if(pf_in) { pf_in >> uw; g.power_w = uw / 1000000.0; }
                }
                std::ifstream plf(base + "power1_cap");
                if(plf){ long long cap; plf >> cap; g.power_max_w = cap / 1000000.0; }
                
                if(!g.detected) { g.name = chip; g.detected = true; }
                break;
            }
        }
    }
    
    // 3. Intel RAPL fallback for power (Calculates Watts via delta over time)
    if(g.detected && g.power_w == 0.0) {
        std::ifstream rapl_f("/sys/class/powercap/intel-rapl:0:1/energy_uj"); // Uncore/GPU
        if(!rapl_f.is_open()) rapl_f.open("/sys/class/powercap/intel-rapl:0:2/energy_uj");
        if(!rapl_f.is_open()) rapl_f.open("/sys/class/powercap/intel-rapl:0/energy_uj"); // Package
        
        if(rapl_f.is_open()) {
            long long curr_uj = 0;
            rapl_f >> curr_uj;
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - prev_rapl_time).count();
            if(prev_rapl_uj > 0 && curr_uj >= prev_rapl_uj && dt > 0.05) {
                g.power_w = (curr_uj - prev_rapl_uj) / 1000000.0 / dt;
            }
            prev_rapl_uj = curr_uj;
            prev_rapl_time = now;
        }
    }
    return g;
}

static CPUStats readCPU(){
    CPUStats s; std::ifstream f("/proc/stat"); std::string line;
    while(std::getline(f,line)){
        if(line.size()<4 || line.substr(0,3)!="cpu") break;
        if(line[3] < '0' || line[3] > '9') continue; // Skip aggregate "cpu " line
        std::istringstream ss(line); std::string tag; CPUCore c;
        ss>>tag>>c.user>>c.nice>>c.system>>c.idle>>c.iowait>>c.irq>>c.softirq>>c.steal;
        s.cores.push_back(c);
    }
    for(int i=0;i<16;i++){
        std::string p="/sys/devices/system/cpu/cpu"+std::to_string(i)+"/cpufreq/scaling_cur_freq";
        std::ifstream ff(p); if(ff){long v;ff>>v;s.freq_mhz=v/1000.0;break;}
    }
    std::ifstream gov("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if(gov) std::getline(gov, s.governor);
    s.governor = trim(s.governor);
    
    for(int i=0; i<10; i++){
        std::string base = "/sys/class/hwmon/hwmon" + std::to_string(i) + "/";
        std::ifstream nf(base + "name");
        if(!nf) continue;
        std::string chip; nf >> chip;
        if(chip.find("coretemp")!=std::string::npos || chip.find("k10temp")!=std::string::npos){
            std::ifstream pf(base + "power1_input");
            if(pf){ long long uw; pf >> uw; s.package_w = uw / 1000000.0; break; }
        }
    }
    return s;
}

static MemStats readMem(){
    MemStats m; std::map<std::string,long> kv; std::ifstream f("/proc/meminfo"); std::string line;
    while(std::getline(f,line)){
        std::istringstream ss(line); std::string k; long v; ss>>k>>v;
        if(!k.empty() && k.back()==':') k.pop_back(); kv[k]=v;
    }
    m.total=kv.count("MemTotal")?kv["MemTotal"]:0; m.free=kv.count("MemFree")?kv["MemFree"]:0;
    m.available=kv.count("MemAvailable")?kv["MemAvailable"]:0; m.buffers=kv.count("Buffers")?kv["Buffers"]:0;
    m.cached=kv.count("Cached")?kv["Cached"]:0; m.swap_total=kv.count("SwapTotal")?kv["SwapTotal"]:0;
    m.swap_free=kv.count("SwapFree")?kv["SwapFree"]:0; return m;
}

static std::vector<Sensor> readSensors(){
    std::vector<Sensor> cpu_s, other_s;
    for(int h=0; h<10; h++){
        std::string base="/sys/class/hwmon/hwmon"+std::to_string(h)+"/";
        std::ifstream nf(base+"name"); if(!nf) continue; std::string chip; nf>>chip;
        bool is_cpu=false;
        if(chip.find("coretemp")!=std::string::npos || chip.find("k10temp")!=std::string::npos) is_cpu=true;
        for(int j=1; j<=8; j++){
            std::ifstream tf(base+"temp"+std::to_string(j)+"_input"); if(!tf) continue;
            int milli; tf>>milli; float tc=milli/1000.0f; if(tc<25 || tc>110) continue;
            std::string lbl=chip;
            std::ifstream lf(base+"temp"+std::to_string(j)+"_label");
            if(lf){ std::getline(lf,lbl); lbl=trim(lbl); } else if(is_cpu) lbl="Core"+std::to_string(j-1);
            if(is_cpu) cpu_s.push_back({lbl,"CPU",tc}); else other_s.push_back({lbl,"SYS",tc});
        }
    }
    std::vector<Sensor> out;
    for(auto& s:cpu_s) { if((int)out.size()>=8) break; out.push_back(s); }
    return out;
}

static std::vector<NetIface> readNet(){
    std::vector<NetIface> wifi, other; std::ifstream f("/proc/net/dev"); std::string line;
    std::getline(f,line); std::getline(f,line);
    while(std::getline(f,line)){
        std::istringstream ss(line); std::string name; ss>>name;
        if(!name.empty() && name.back()==':') name.pop_back(); if(name=="lo") continue;
        NetIface n; n.name=name; long long dummy;
        ss>>n.rx_bytes>>dummy>>dummy>>dummy>>dummy>>dummy>>dummy>>dummy>>n.tx_bytes>>dummy;
        if(name.find("w")==0){ n.primary=true; wifi.push_back(n); } else other.push_back(n);
    }
    std::vector<NetIface> v = wifi; v.insert(v.end(), other.begin(), other.end());
    if(wifi.empty() && !v.empty()) v[0].primary=true; return v;
}

static std::vector<DiskInfo> readDisks(){
    std::vector<DiskInfo> disks; std::ifstream f("/proc/mounts"); std::string line; std::vector<std::string> seen;
    static const std::vector<std::string> skip_fs={"tmpfs","devtmpfs","sysfs","proc","cgroup","overlay","squashfs"};
    while(std::getline(f,line) && (int)disks.size()<4){
        std::istringstream ss(line); std::string dev,mount,fs; ss>>dev>>mount>>fs;
        bool skip=false; for(auto& s:skip_fs) if(fs==s) skip=true; if(skip) continue;
        if(mount.substr(0,4)=="/sys" || mount.substr(0,5)=="/proc" || mount.substr(0,4)=="/dev" || mount.substr(0,4)=="/run") continue;
        if(std::find(seen.begin(),seen.end(),mount)!=seen.end()) continue; seen.push_back(mount);
        struct statvfs sv; if(statvfs(mount.c_str(), &sv)!=0) continue;
        DiskInfo d; d.device=dev; d.mount=mount;
        d.total=(unsigned long long)sv.f_blocks * sv.f_frsize; d.used=d.total - (unsigned long long)sv.f_bfree * sv.f_frsize;
        if(d.total > 1024*1024) disks.push_back(d);
    } return disks;
}

static std::map<std::string,DiskIO> readDiskIO(){
    std::map<std::string,DiskIO> m; std::ifstream f("/proc/diskstats"); std::string line;
    while(std::getline(f,line)){
        std::istringstream ss(line); int maj,min; std::string name; ss>>maj>>min>>name;
        long long dummy,rs,ws; for(int i=0;i<3;i++) ss>>dummy; ss>>rs; for(int i=0;i<3;i++) ss>>dummy; ss>>ws;
        if(name.find("sd")==0 || name.find("nvme")==0 || name.find("mmc")==0) m[name]={rs*512, ws*512};
    } return m;
}

static HWInfo readHW(const SysMeta& meta){
    HWInfo hw; hw.loaded=true; hw.cpu=meta.cpu_model; hw.cores=meta.cores;
    std::ifstream df("/sys/class/dmi/id/product_name");
    if(df) std::getline(df,hw.model);
    hw.model=trim(hw.model); if(hw.model.empty()) hw.model="Unknown";
    std::ifstream cf("/proc/cpuinfo"); std::string line; int thr=0;
    while(std::getline(cf,line)){
        if(line.find("processor")!=std::string::npos) thr++;
        if(line.find("cpu cores")!=std::string::npos){ auto p=line.find(':'); if(p!=std::string::npos) hw.cores=std::stoi(trim(line.substr(p+1))); }
    }
    hw.threads=thr; if(hw.cores==0) hw.cores=thr;
    std::ifstream mf("/proc/meminfo");
    while(std::getline(mf,line)){ if(line.find("MemTotal")!=std::string::npos){ auto p=line.find(':'); if(p!=std::string::npos) hw.ram_gb=std::stol(trim(line.substr(p+1)))/1024/1024; } }
    std::ifstream kf("/proc/version"); std::getline(kf,line);
    auto p1=line.find("version "); if(p1!=std::string::npos){ line=line.substr(p1+8); auto p2=line.find(' '); if(p2!=std::string::npos) hw.kernel=line.substr(0,p2); }
    DIR* dir=opendir("/sys/class/net");
    if(dir){ struct dirent* e; while((e=readdir(dir))){ std::string n=e->d_name; if(n=="lo") continue; if(n.find("wl")==0){ hw.net_iface=n; break; } if(hw.net_iface.empty()) hw.net_iface=n; } closedir(dir); }
    if(!hw.net_iface.empty()){
        std::ifstream macf("/sys/class/net/"+hw.net_iface+"/address"); if(macf) std::getline(macf,hw.mac);
    }
    if(hw.mac.empty()) hw.mac="N/A";
    return hw;
}

static SysMeta readMeta(){
    SysMeta m; std::ifstream hf("/etc/hostname"); std::getline(hf,m.hostname); if(m.hostname.empty()) m.hostname="localhost";
    std::ifstream cf("/proc/cpuinfo"); std::string line;
    while(std::getline(cf,line)){ if(line.find("model name")!=std::string::npos){ auto pos=line.find(':'); if(pos!=std::string::npos){ m.cpu_model=trim(line.substr(pos+2)); break; } } }
    if((int)m.cpu_model.size()>40) m.cpu_model=m.cpu_model.substr(0,38)+"..";
    m.cores=sysconf(_SC_NPROCESSORS_ONLN); return m;
}

static std::map<int,long long> g_prev_proc_t;
static std::vector<ProcInfo> readProcs(long long cpu_delta){
    std::vector<ProcInfo> procs; DIR* dir=opendir("/proc"); if(!dir) return procs; struct dirent* e;
    while((e=readdir(dir))){
        bool is_pid=true; for(char* p=e->d_name; *p; p++) if(*p<'0'||*p>'9'){is_pid=false;break;} if(!is_pid) continue;
        int pid=atoi(e->d_name); std::ifstream sf("/proc/"+std::string(e->d_name)+"/stat"); if(!sf) continue;
        std::string line; std::getline(sf,line); auto p1=line.find('('), p2=line.rfind(')'); if(p1==std::string::npos || p2==std::string::npos) continue;
        ProcInfo pi; pi.pid=pid; pi.name=line.substr(p1+1,p2-p1-1);
        std::istringstream ss(line.substr(p2+2)); char st; int di; long long dull;
        ss>>st>>di>>di>>di>>di>>di>>dull>>dull>>dull>>dull>>dull>>pi.cpu_ticks; pi.state=st;
        std::ifstream msf("/proc/"+std::string(e->d_name)+"/status"); std::string ml;
        while(std::getline(msf,ml)){ if(ml.find("VmRSS:")==0){ std::istringstream mss(ml); std::string k; mss>>k>>pi.mem_kb; break; } }
        if(g_prev_proc_t.count(pid) && cpu_delta>0) pi.cpu_pct=100.0*(pi.cpu_ticks-g_prev_proc_t[pid])/cpu_delta;
        g_prev_proc_t[pid]=pi.cpu_ticks; procs.push_back(pi);
    } closedir(dir);
    std::sort(procs.begin(),procs.end(),[](const ProcInfo&a,const ProcInfo&b){ if(std::abs(a.cpu_pct-b.cpu_pct)>0.05) return a.cpu_pct>b.cpu_pct; return a.mem_kb>b.mem_kb; });
    if((int)procs.size()>100) procs.resize(100); return procs;
}

// ─────────────────────────────────────────────────────────────────────
//  STATE UPDATE
// ─────────────────────────────────────────────────────────────────────
static void updateState(AppState& st){
    st.cpu_prev=st.cpu_curr; st.net_prev=st.net_curr; st.disk_io_prev=st.disk_io_curr;
    st.cpu_curr=readCPU(); st.mem=readMem(); st.sensors=readSensors(); st.net_curr=readNet();
    st.disks=readDisks(); st.disk_io_curr=readDiskIO(); st.gpu=readGPU(); 
    st.meta.uptime=readMeta().uptime;
    
    if(!st.cpu_prev.cores.empty() && !st.cpu_curr.cores.empty()){
        int n=std::min(st.cpu_prev.cores.size(),st.cpu_curr.cores.size()); st.cpu_pct.resize(n);
        double total_pct = 0;
        for(int i=0;i<n;i++){
            long long tot=st.cpu_curr.cores[i].total()-st.cpu_prev.cores[i].total();
            long long idle=(st.cpu_curr.cores[i].idle+st.cpu_curr.cores[i].iowait)-(st.cpu_prev.cores[i].idle+st.cpu_prev.cores[i].iowait);
            st.cpu_pct[i]=tot>0 ? 100.0*(tot-idle)/tot : 0.0;
            total_pct += st.cpu_pct[i];
        }
        st.cpu_hist.push_back(total_pct / n); if((int)st.cpu_hist.size()>60) st.cpu_hist.pop_front();
    }
    
    st.gpu_hist.push_back(st.gpu.usage_pct); if((int)st.gpu_hist.size()>40) st.gpu_hist.pop_front();

    double dt=g_cfg.update_ms/1000.0;
    for(auto& c:st.net_curr){ for(auto& p:st.net_prev){ if(c.name==p.name){
        c.rx_rate=(c.rx_bytes-p.rx_bytes)/dt; c.tx_rate=(c.tx_bytes-p.tx_bytes)/dt;
        auto& rxh=st.rx_hist[c.name]; rxh.push_back(c.rx_rate); if((int)rxh.size()>40) rxh.pop_front();
        auto& txh=st.tx_hist[c.name]; txh.push_back(c.tx_rate); if((int)txh.size()>40) txh.pop_front(); break;
    }}}
    for(auto& d:st.disks){
        std::string dev=d.device; if(dev.find("/dev/")==0) dev=dev.substr(5);
        while(!dev.empty() && dev.back()>='0' && dev.back()<='9' && dev.find("nvme")==std::string::npos) dev.pop_back();
        auto it=st.disk_io_curr.find(dev), prev=st.disk_io_prev.find(dev);
        if(it!=st.disk_io_curr.end() && prev!=st.disk_io_prev.end()){ d.read_rate=(it->second.rb - prev->second.rb)/dt; d.write_rate=(it->second.wb - prev->second.wb)/dt; }
    }
    long long cpu_delta=0; if(!st.cpu_prev.cores.empty() && !st.cpu_curr.cores.empty()) cpu_delta=st.cpu_curr.cores[0].total()-st.cpu_curr.cores[0].total();
    if(g_cfg.proc && cpu_delta>0) st.procs=readProcs(cpu_delta);
}

// ─────────────────────────────────────────────────────────────────────
//  DRAWING PRIMITIVES
// ─────────────────────────────────────────────────────────────────────
static void drawBox(int y, int x, int h, int w, const std::string& title) {
    if(h<3 || w<6) return;
    attron(COLOR_PAIR(C_BORDER));
    mvaddch(y,x, ACS_ULCORNER); for(int i=x+1;i<x+w-1;i++) mvaddch(y,i, ACS_HLINE); mvaddch(y,x+w-1, ACS_URCORNER);
    for(int i=y+1;i<y+h-1;i++){ mvaddch(i,x, ACS_VLINE); mvaddch(i,x+w-1, ACS_VLINE); }
    mvaddch(y+h-1,x, ACS_LLCORNER); for(int i=x+1;i<x+w-1;i++) mvaddch(y+h-1,i, ACS_HLINE); mvaddch(y+h-1,x+w-1, ACS_LRCORNER);
    attroff(COLOR_PAIR(C_BORDER));
    if(!title.empty() && w>8){
        std::string t=" "+title+" "; if((int)t.size()>w-6) t=t.substr(0,w-6);
        attron(COLOR_PAIR(C_CYAN)|A_BOLD); mvaddstr(y,x+2,t.c_str()); attroff(COLOR_PAIR(C_CYAN)|A_BOLD);
    }
}

static void drawCompactBar(int y, int x, int w, double pct) {
    int filled = (int)(pct / 100.0 * w);
    filled = std::min(filled, w);
    for(int i=0; i<filled; i++){
        int c = (i < w/2) ? C_USAGE_1 : C_USAGE_2;
        attron(COLOR_PAIR(c)|A_BOLD); mvaddstr(y, x+i, "▮"); attroff(COLOR_PAIR(c)|A_BOLD);
    }
    attron(COLOR_PAIR(C_DIM));
    for(int i=filled; i<w; i++) mvaddstr(y, x+i, "▯");
    attroff(COLOR_PAIR(C_DIM));
}

// ─────────────────────────────────────────────────────────────────────
//  SIGNAL UI
// ─────────────────────────────────────────────────────────────────────
struct SignalOption { int num; const char* name; const char* desc; };
static const SignalOption SIGNALS[] = {
    {15, "SIGTERM", "Terminate"}, {9, "SIGKILL", "Kill"}, {2, "SIGINT", "Interrupt"},
    {1, "SIGHUP", "Hangup"}, {18, "SIGCONT", "Continue"}, {19, "SIGSTOP", "Stop"},
};
static const int NUM_SIGNALS = 6;
static int g_signal_selected = 0;

static void drawSignalUI(const AppState& st, int rows, int cols){
    if(g_cfg.proc_selected >= (int)st.procs.size()) return;
    const auto& proc = st.procs[g_cfg.proc_selected];
    int ui_w = cols - 4; int ui_h = rows - 6; int ui_y = 2; int ui_x = 2; int left_w = ui_w / 3;

    attron(COLOR_PAIR(C_BORDER) | A_BOLD);
    mvaddch(ui_y, ui_x, ACS_ULCORNER); mvaddch(ui_y, ui_x+ui_w-1, ACS_URCORNER);
    mvaddch(ui_y+ui_h-1, ui_x, ACS_LLCORNER); mvaddch(ui_y+ui_h-1, ui_x+ui_w-1, ACS_LRCORNER);
    for(int i=ui_x+1; i<ui_x+ui_w-1; i++){ mvaddch(ui_y, i, ACS_HLINE); mvaddch(ui_y+ui_h-1, i, ACS_HLINE); }
    for(int i=ui_y+1; i<ui_y+ui_h-1; i++){ mvaddch(i, ui_x, ACS_VLINE); mvaddch(i, ui_x+ui_w-1, ACS_VLINE); mvaddch(i, ui_x+left_w, ACS_VLINE); }
    attroff(COLOR_PAIR(C_BORDER) | A_BOLD);

    attron(COLOR_PAIR(C_CYAN) | A_BOLD); mvaddstr(ui_y, ui_x+2, " SEND SIGNAL TO PROCESS "); attroff(COLOR_PAIR(C_CYAN) | A_BOLD);
    attron(COLOR_PAIR(C_TEXT) | A_BOLD); mvaddstr(ui_y+2, ui_x+2, "SIGNALS"); attroff(COLOR_PAIR(C_TEXT) | A_BOLD);

    for(int i=0; i<NUM_SIGNALS && i<ui_h-5; i++){
        int y = ui_y + 4 + i; bool selected = (i == g_signal_selected);
        if(selected){ attron(COLOR_PAIR(C_SELECTED)); mvaddstr(y, ui_x+1, "▸ "); attroff(COLOR_PAIR(C_SELECTED)); }
        else mvaddstr(y, ui_x+1, "  ");
        attron(selected ? (COLOR_PAIR(C_SELECTED)|A_BOLD) : COLOR_PAIR(C_DIM));
        mvprintw(y, ui_x+3, "%s (%d)", SIGNALS[i].name, SIGNALS[i].num);
        attroff(selected ? (COLOR_PAIR(C_SELECTED)|A_BOLD) : COLOR_PAIR(C_DIM));
    }

    attron(COLOR_PAIR(C_TEXT) | A_BOLD); mvaddstr(ui_y+2, ui_x+left_w+2, "PROCESS INFO"); attroff(COLOR_PAIR(C_TEXT) | A_BOLD);
    int info_y = ui_y + 4; int info_x = ui_x + left_w + 2;
    mvprintw(info_y++, info_x, "Name: %s", proc.name.c_str());
    mvprintw(info_y++, info_x, "PID: %d", proc.pid);
    mvprintw(info_y++, info_x, "State: %c", proc.state);
    mvprintw(info_y++, info_x, "CPU%%: %.1f%%", proc.cpu_pct);
    mvprintw(info_y++, info_x, "Memory: %ld MB", proc.mem_kb/1024);
}

static int handleSignalUI(AppState& st){
    if(g_cfg.proc_selected >= (int)st.procs.size()) return 0;
    const auto& proc = st.procs[g_cfg.proc_selected];
    nocbreak(); cbreak(); echo(); keypad(stdscr, TRUE);
    int sig = 0; bool done = false;
    while(!done){
        drawSignalUI(st, LINES, COLS); refresh();
        int ch = getch();
        switch(ch){
            case KEY_UP: g_signal_selected--; if(g_signal_selected < 0) g_signal_selected = NUM_SIGNALS - 1; break;
            case KEY_DOWN: g_signal_selected++; if(g_signal_selected >= NUM_SIGNALS) g_signal_selected = 0; break;
            case '\n': case KEY_ENTER: sig = SIGNALS[g_signal_selected].num; done = true; break;
            case 'q': case 'Q': case 27: sig = 0; done = true; break;
        }
    }
    noecho(); nodelay(stdscr, TRUE);
    if(sig > 0) kill(proc.pid, sig);
    g_cfg.signal_mode = false; g_signal_selected = 0;
    return sig;
}

// ─────────────────────────────────────────────────────────────────────
//  PANEL RENDERERS
// ─────────────────────────────────────────────────────────────────────
static void pCPU(const AppState& st, int y, int x, int h, int w) {
    drawBox(y, x, h, w, "CPU OVERVIEW & TEMPS");
    int r = y + 1;
    
    attron(COLOR_PAIR(C_TEXT)|A_BOLD); mvaddstr(r, x+2, st.meta.cpu_model.substr(0, w/2).c_str()); attroff(COLOR_PAIR(C_TEXT)|A_BOLD);
    attron(COLOR_PAIR(C_CYAN)); mvprintw(r, x+w-20, "%4.0fMHz", st.cpu_curr.freq_mhz); attroff(COLOR_PAIR(C_CYAN));
    r++;
    
    double overall = 0;
    if(!st.cpu_pct.empty()) for(auto p : st.cpu_pct) overall += p;
    overall = st.cpu_pct.empty() ? 0 : overall / st.cpu_pct.size();
    
    attron(COLOR_PAIR(C_USAGE_1)); mvaddstr(r, x+2, "Use:"); attroff(COLOR_PAIR(C_USAGE_1));
    attron(COLOR_PAIR(usageColor(overall))|A_BOLD); mvprintw(r, x+7, "%3.0f%%", overall); attroff(COLOR_PAIR(usageColor(overall))|A_BOLD);
    
    attron(COLOR_PAIR(C_WATT_1)); mvaddstr(r, x+14, "Pwr:"); attroff(COLOR_PAIR(C_WATT_1));
    attron(COLOR_PAIR(C_WATT_2)|A_BOLD); mvprintw(r, x+19, "%4.1fW", st.cpu_curr.package_w); attroff(COLOR_PAIR(C_WATT_2)|A_BOLD);

    attron(COLOR_PAIR(C_GREEN)); mvaddstr(r, x+30, "Gov:"); attroff(COLOR_PAIR(C_GREEN));
    attron(COLOR_PAIR(C_TEXT)|A_BOLD); mvaddstr(r, x+35, st.cpu_curr.governor.c_str()); attroff(COLOR_PAIR(C_TEXT)|A_BOLD);
    r += 2;

    int num_cores = st.cpu_pct.size();
    if (num_cores == 0) return;
    int grid_cols = (num_cores > 16) ? 4 : (num_cores > 8) ? 3 : 2;
    if (w < 60) grid_cols = 1;
    
    int col_w = (w - 4) / grid_cols;
    int bar_w = col_w - 12;
    if (bar_w < 4) bar_w = 4;

    for(int i=0; i<num_cores; i++){
        int col = i % grid_cols;
        int row_offset = i / grid_cols;
        int cx = x + 2 + col * col_w;
        int cy = r + row_offset;
        
        if (cy >= y + h - 2) break;

        double pct = st.cpu_pct[i];
        attron(COLOR_PAIR(C_DIM)); mvprintw(cy, cx, "C%-2d", i); attroff(COLOR_PAIR(C_DIM));
        drawCompactBar(cy, cx+4, bar_w, pct);
        attron(COLOR_PAIR(usageColor(pct))|A_BOLD); mvprintw(cy, cx+4+bar_w+1, "%3.0f%%", pct); attroff(COLOR_PAIR(usageColor(pct))|A_BOLD);
    }
    
    int temp_r = y + h - 2;
    if(!st.sensors.empty()){
        attron(COLOR_PAIR(C_BORDER)); for(int i=x+1; i<x+w-1; i++) mvaddch(temp_r-1, i, ACS_HLINE); attroff(COLOR_PAIR(C_BORDER));
        int tx = x + 2;
        for(auto& s : st.sensors){
            if(tx + 15 > x + w - 2) break;
            attron(COLOR_PAIR(tempColor(s.temp))|A_BOLD);
            mvprintw(temp_r, tx, "%s:%.0f°C", s.label.substr(0,6).c_str(), s.temp);
            attroff(COLOR_PAIR(tempColor(s.temp))|A_BOLD);
            tx += 14;
        }
    }
}

static void pGPU(const AppState& st, int y, int x, int h, int w) {
    drawBox(y, x, h, w, "GPU STATS");
    int r = y + 1;
    
    attron(COLOR_PAIR(C_TEXT)|A_BOLD); mvaddstr(r, x+2, st.gpu.name.substr(0, w-4).c_str()); attroff(COLOR_PAIR(C_TEXT)|A_BOLD); r++;
    
    attron(COLOR_PAIR(C_USAGE_1)); mvaddstr(r, x+2, "Util:"); attroff(COLOR_PAIR(C_USAGE_1));
    drawCompactBar(r, x+8, w/3, st.gpu.usage_pct);
    attron(COLOR_PAIR(usageColor(st.gpu.usage_pct))|A_BOLD); mvprintw(r, x+8+w/3+1, "%3d%%", st.gpu.usage_pct); attroff(COLOR_PAIR(usageColor(st.gpu.usage_pct))|A_BOLD);
    r++;
    
    attron(COLOR_PAIR(C_WATT_1)); mvaddstr(r, x+2, "Pwr:"); attroff(COLOR_PAIR(C_WATT_1));
    double pwr_pct = (st.gpu.power_max_w > 0) ? (st.gpu.power_w / st.gpu.power_max_w * 100.0) : 0;
    drawCompactBar(r, x+8, w/3, pwr_pct);
    attron(COLOR_PAIR(C_WATT_2)|A_BOLD); mvprintw(r, x+8+w/3+1, "%.1fW/%.0fW", st.gpu.power_w, st.gpu.power_max_w); attroff(COLOR_PAIR(C_WATT_2)|A_BOLD);
    r++;

    attron(COLOR_PAIR(C_RED)); mvaddstr(r, x+2, "Temp:"); attroff(COLOR_PAIR(C_RED));
    attron(COLOR_PAIR(tempColor(st.gpu.temp_c))|A_BOLD); mvprintw(r, x+8, "%.1f °C", st.gpu.temp_c); attroff(COLOR_PAIR(tempColor(st.gpu.temp_c))|A_BOLD);
}

static void pMem(const AppState& st, int y, int x, int h, int w) {
    drawBox(y, x, h, w, "MEMORY");
    int r = y + 1;
    double rpct = st.mem.total>0 ? 100.0*st.mem.used()/st.mem.total : 0;
    attron(COLOR_PAIR(C_USAGE_1)|A_BOLD); mvaddstr(r, x+2, "RAM"); attroff(COLOR_PAIR(C_USAGE_1)|A_BOLD);
    drawCompactBar(r, x+6, w-12, rpct);
    attron(COLOR_PAIR(C_TEXT)); mvprintw(r, x+w-5, "%2.0f%%", rpct); attroff(COLOR_PAIR(C_TEXT)); r++;
    attron(COLOR_PAIR(C_DIM)); mvprintw(r, x+2, "%s / %s", fmtBytes(st.mem.used()*1024).c_str(), fmtBytes(st.mem.total*1024).c_str()); attroff(COLOR_PAIR(C_DIM));
}

static void pDisk(const AppState& st, int y, int x, int h, int w) {
    drawBox(y, x, h, w, "DISK I/O");
    int r = y + 1;
    for(auto& d:st.disks){
        if(r >= y+h-1) break;
        double dpct = d.total>0 ? 100.0*d.used/d.total : 0;
        std::string mnt = d.mount; if((int)mnt.size()>6) mnt=mnt.substr(0,5)+".";
        attron(COLOR_PAIR(C_TEXT)); mvaddstr(r, x+2, mnt.c_str()); attroff(COLOR_PAIR(C_TEXT));
        drawCompactBar(r, x+9, w-15, dpct);
        attron(COLOR_PAIR(C_CYAN)); mvprintw(r, x+w-5, "%2.0f%%", dpct); attroff(COLOR_PAIR(C_CYAN)); r++;
        attron(COLOR_PAIR(C_DIM)); mvprintw(r, x+4, "R:%-8s W:%s", fmtRate(d.read_rate).c_str(), fmtRate(d.write_rate).c_str()); attroff(COLOR_PAIR(C_DIM)); r++;
    }
}

static void pNet(const AppState& st, int y, int x, int h, int w) {
    drawBox(y, x, h, w, "NETWORK");
    int r = y + 1;
    for(auto& iface:st.net_curr){
        if(r >= y+h-1) break;
        attron(COLOR_PAIR(C_TEXT)|A_BOLD); mvaddstr(r, x+2, ((iface.primary?"★ ":"") + iface.name).substr(0, w/2).c_str()); attroff(COLOR_PAIR(C_TEXT)|A_BOLD);
        attron(COLOR_PAIR(C_GREEN)); mvaddstr(r, x+w/2, "▼"); attroff(COLOR_PAIR(C_GREEN));
        attron(COLOR_PAIR(C_TEXT)); mvaddstr(r, x+w/2+2, fmtRate(iface.rx_rate).c_str()); attroff(COLOR_PAIR(C_TEXT));
        attron(COLOR_PAIR(C_RED)); mvaddstr(r, x+w-12, "▲"); attroff(COLOR_PAIR(C_RED));
        attron(COLOR_PAIR(C_TEXT)); mvaddstr(r, x+w-10, fmtRate(iface.tx_rate).c_str()); attroff(COLOR_PAIR(C_TEXT)); r++;
    }
}

static void pHW(const AppState& st, int y, int x, int h, int w) {
    drawBox(y, x, h, w, "HARDWARE INFO");
    int r = y + 1;
    auto row = [&](const std::string& lbl, const std::string& val){
        if(r >= y+h-1) return;
        attron(COLOR_PAIR(C_DIM)); mvaddstr(r, x+2, (lbl+":").c_str()); attroff(COLOR_PAIR(C_DIM));
        attron(COLOR_PAIR(C_TEXT)|A_BOLD); mvaddstr(r, x+12, val.substr(0, w-14).c_str()); attroff(COLOR_PAIR(C_TEXT)|A_BOLD);
        r++;
    };
    row("Model", st.hw.model);
    row("CPU", st.hw.cpu);
    row("GPU", st.gpu.detected ? st.gpu.name : "None/iGPU");
    row("RAM", std::to_string(st.hw.ram_gb)+"GB");
    row("Cores", std::to_string(st.hw.cores)+"C / "+std::to_string(st.hw.threads)+"T");
    row("Kernel", st.hw.kernel);
    row("Net", st.hw.net_iface);
}

static void pProc(const AppState& st, int y, int x, int h, int w, int scroll, int selected) {
    drawBox(y, x, h, w, "PROCESSES (Click/K to Kill)");
    int r = y + 1;
    attron(COLOR_PAIR(C_CYAN)|A_BOLD|A_UNDERLINE);
    mvaddstr(r, x+2, "PID      NAME                 CPU%    MEM       STATE");
    attroff(COLOR_PAIR(C_CYAN)|A_BOLD|A_UNDERLINE); r++;
    
    int shown = 0;
    for(int i=scroll; i<(int)st.procs.size() && r<y+h-1; i++, shown++){
        auto& p = st.procs[i];
        bool is_sel = (i == selected);
        if(is_sel) attron(COLOR_PAIR(C_SELECTED));
        std::string name = p.name; if((int)name.size()>20) name=name.substr(0,19)+".";
        mvprintw(r, x+2, "%-8d %-20s %5.1f%%   %-8s  %c", p.pid, name.c_str(), p.cpu_pct, fmtBytes(p.mem_kb*1024).c_str(), p.state);
        if(is_sel) attroff(COLOR_PAIR(C_SELECTED));
        r++;
    }
}

// ─────────────────────────────────────────────────────────────────────
//  MAIN LAYOUT & LOOP
// ─────────────────────────────────────────────────────────────────────
int main(){
    setlocale(LC_ALL,"");
    signal(SIGWINCH,[](int){g_resize=true;}); signal(SIGINT,[](int){g_quit=true;}); signal(SIGTERM,[](int){g_quit=true;});
    initscr(); cbreak(); noecho(); keypad(stdscr,TRUE); nodelay(stdscr,TRUE); curs_set(0); timeout(50);
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL); mouseinterval(0);
    
    if(!has_colors()){endwin();fprintf(stderr,"No color support.\n");return 1;}
    start_color(); use_default_colors(); applyColors(); 
    bkgd(COLOR_PAIR(C_BG)); wbkgd(stdscr, COLOR_PAIR(C_BG));

    AppState st; st.meta=readMeta(); st.hw=readHW(st.meta); st.cpu_curr=readCPU(); st.net_curr=readNet(); st.disk_io_curr=readDiskIO(); st.sensors=readSensors(); st.gpu=readGPU();
    usleep(150000); updateState(st);

    int loop_ms=50, tick_max=g_cfg.update_ms/loop_ms, tick=tick_max;

    while(!g_quit){
        if(g_resize){ endwin(); refresh(); applyColors(); bkgd(COLOR_PAIR(C_BG)); wbkgd(stdscr, COLOR_PAIR(C_BG)); g_resize=false; continue; }
        if(tick>=tick_max){ updateState(st); tick=0; } tick++;
        
        int rows,cols; getmaxyx(stdscr,rows,cols); 
        bkgd(COLOR_PAIR(C_BG)); wbkgd(stdscr, COLOR_PAIR(C_BG));
        erase(); 
        
        attron(COLOR_PAIR(C_CYAN)|A_BOLD); mvaddstr(0, 2, "◈ TTOP v4.0.2"); attroff(COLOR_PAIR(C_CYAN)|A_BOLD);
        attron(COLOR_PAIR(C_DIM)); mvaddstr(0, 16, ("| "+st.meta.hostname).c_str()); attroff(COLOR_PAIR(C_DIM));
        attron(COLOR_PAIR(C_GREEN)); mvprintw(0, cols-25, "%.1f FPS | Upd: %dms", g_fps.fps, g_cfg.update_ms); attroff(COLOR_PAIR(C_GREEN));
        attron(COLOR_PAIR(C_BORDER)); for(int i=0; i<cols; i++) mvaddch(1, i, ACS_HLINE); attroff(COLOR_PAIR(C_BORDER));

        int top_h = rows * 0.45;
        int mid_h = rows * 0.15;
        int bot_h = rows - top_h - mid_h - 3; 
        int half_w = cols / 2;

        if(st.gpu.detected && g_cfg.gpu){
            pCPU(st, 2, 0, top_h, half_w);
            pGPU(st, 2, half_w, top_h, cols - half_w);
        } else {
            pCPU(st, 2, 0, top_h, cols);
        }
        
        int mid_y = 2 + top_h;
        int third_w = cols / 3;
        if(g_cfg.mem) pMem(st, mid_y, 0, mid_h, third_w);
        if(g_cfg.disk) pDisk(st, mid_y, third_w, mid_h, third_w);
        if(g_cfg.net) pNet(st, mid_y, third_w * 2, mid_h, cols - (third_w * 2));

        int bot_y = mid_y + mid_h;
        if(g_cfg.hw) pHW(st, bot_y, 0, bot_h, half_w);
        if(g_cfg.proc) pProc(st, bot_y, half_w, bot_h, cols - half_w, g_cfg.proc_scroll, g_cfg.proc_selected);

        refresh(); g_fps.tick();
        
        int ch=getch();
        if(ch==KEY_MOUSE){
            MEVENT mevent;
            if(getmouse(&mevent)==OK && (mevent.bstate & BUTTON1_CLICKED)){
                int bot_y_proc = mid_y + mid_h;
                if(mevent.x >= half_w && mevent.y >= bot_y_proc + 2 && mevent.y < rows - 1){
                    int clicked_idx = g_cfg.proc_scroll + (mevent.y - (bot_y_proc + 2));
                    if(clicked_idx >= 0 && clicked_idx < (int)st.procs.size()){
                        g_cfg.proc_selected = clicked_idx;
                    }
                }
            }
        } else if(ch!=ERR){
            switch(ch){
                case '1': g_cfg.cpu=!g_cfg.cpu; break;
                case '2': g_cfg.mem=!g_cfg.mem; break;
                case '3': g_cfg.net=!g_cfg.net; break;
                case '4': g_cfg.disk=!g_cfg.disk; break;
                case '5': g_cfg.gpu=!g_cfg.gpu; break;
                case '6': g_cfg.proc=!g_cfg.proc; break;
                case 'e': case 'E': cycleGovernor(); break;
                case 'k': case 'K':
                    if(g_cfg.proc && !st.procs.empty() && g_cfg.proc_selected < (int)st.procs.size()){
                        kill(st.procs[g_cfg.proc_selected].pid, SIGTERM);
                    } break;
                case 'q': case 'Q': g_quit=true; break;
                case '+': case '=': 
                    g_cfg.update_ms=std::max(100, g_cfg.update_ms-100); 
                    tick_max=g_cfg.update_ms/loop_ms; 
                    break;
                case '-': 
                    g_cfg.update_ms=std::min(5000, g_cfg.update_ms+100); 
                    tick_max=g_cfg.update_ms/loop_ms; 
                    break;
                case KEY_UP: if(g_cfg.proc_scroll>0) g_cfg.proc_scroll--; if(g_cfg.proc_selected>0) g_cfg.proc_selected--; break;
                case KEY_DOWN: if(g_cfg.proc_scroll<(int)st.procs.size()-1) g_cfg.proc_scroll++; if(g_cfg.proc_selected<(int)st.procs.size()-1) g_cfg.proc_selected++; break;
                case KEY_RESIZE: g_resize=true; break;
            }
        }
        usleep(loop_ms*1000);
    }
    endwin(); printf("\n ◈ ttop v4.0.2 — goodbye\n\n"); return 0;
}
