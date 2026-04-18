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

//  COLOR PAIRS
#define C_DEFAULT   1
#define C_DIM       2
#define C_RED       3
#define C_BLUE      4
#define C_PURPLE    5
#define C_GREEN     6
#define C_YELLOW    7
#define C_CYAN      8
#define C_HBAR      9
#define C_SELECTED 10
#define C_BORDER   11
#define C_TITLE    12
#define C_GRAD_G   13
#define C_GRAD_B   14
#define C_GRAD_R   15
#define C_BAR_EMPTY 16
#define C_WARNING  17

//  THEMES
enum Theme { TH_NEON=0, TH_FROST=1, TH_MIDNIGHT=2 };
static const char* THEME_NAMES[] = {"NEON","FROST","MIDNIGHT"};

struct ThemeDef { int border, title, accent; };
static const ThemeDef THEME_DEFS[] = {
    { COLOR_MAGENTA, COLOR_MAGENTA, COLOR_BLUE },
    { COLOR_CYAN,    COLOR_BLUE,    COLOR_CYAN },
    { COLOR_RED,     COLOR_RED,     COLOR_YELLOW },
};
static Theme g_theme = TH_NEON;

static void applyTheme() {
    const auto& t = THEME_DEFS[g_theme];
    init_pair(C_DEFAULT,   COLOR_WHITE,   COLOR_BLACK);
    init_pair(C_DIM,       COLOR_WHITE,   COLOR_BLACK);
    init_pair(C_RED,       COLOR_RED,     COLOR_BLACK);
    init_pair(C_BLUE,      COLOR_BLUE,    COLOR_BLACK);
    init_pair(C_PURPLE,    COLOR_MAGENTA, COLOR_BLACK);
    init_pair(C_GREEN,     COLOR_GREEN,   COLOR_BLACK);
    init_pair(C_YELLOW,    COLOR_YELLOW,  COLOR_BLACK);
    init_pair(C_CYAN,      COLOR_CYAN,    COLOR_BLACK);
    init_pair(C_HBAR,      COLOR_WHITE,   t.border);
    init_pair(C_SELECTED,  COLOR_BLACK,   t.border);
    init_pair(C_BORDER,    t.border,      COLOR_BLACK);
    init_pair(C_TITLE,     t.title,       COLOR_BLACK);
    init_pair(C_GRAD_G,    COLOR_GREEN,   COLOR_BLACK);
    init_pair(C_GRAD_B,    COLOR_BLUE,    COLOR_BLACK);
    init_pair(C_GRAD_R,    COLOR_RED,     COLOR_BLACK);
    init_pair(C_BAR_EMPTY, COLOR_WHITE,   COLOR_BLACK);
    init_pair(C_WARNING,   COLOR_YELLOW,  COLOR_BLACK);
}

//  GLOBALS
static volatile bool g_resize = false;
static volatile bool g_quit   = false;

static struct {
    bool cpu=true, mem=true, net=true, disk=true, proc=true, hw=true;
    bool graphs=true;
    int  update_ms=500;
    int  proc_scroll=0;
    int  proc_selected=0;      // Selected process row
    bool proc_fullscreen=false;
    bool signal_mode=false;    // Signal send mode active
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

//  DATA STRUCTURES
struct CPUCore {
    long long user=0,nice=0,system=0,idle=0,iowait=0,irq=0,softirq=0,steal=0;
    long long total() const {return user+nice+system+idle+iowait+irq+softirq+steal;}
};
struct CPUStats { std::vector<CPUCore> cores; double freq_mhz=0; };

struct MemStats {
    long total=0,free=0,available=0,buffers=0,cached=0,sreclaimable=0;
    long swap_total=0,swap_free=0;
    long used()      const {return total-available;}
    long swap_used() const {return swap_total-swap_free;}
};

struct MemTypeInfo {
    std::string type;      // DDR3, DDR4, DDR5
    std::string speed;     // 1333MT/s, 2400MT/s
    std::string form;      // SODIMM, DIMM
    bool detected=false;
};

struct Sensor   { std::string label,type; float temp=0; };
struct NetIface {
    std::string name;
    long long rx_bytes=0,tx_bytes=0;
    double rx_rate=0,tx_rate=0;
    bool primary=false;
};
struct DiskInfo {
    std::string mount,device;
    unsigned long long total=0,used=0;
    double read_rate=0,write_rate=0;
};
struct DiskIO   { long long rb=0,wb=0; };
struct BattInfo { bool present=false,charging=false,full=false; int pct=0; };
struct ProcInfo {
    int pid=0; std::string name; char state=' ';
    long mem_kb=0; long long cpu_ticks=0; double cpu_pct=0;
    std::string user;
};
struct HWInfo {
    std::string model,cpu,gpu,kernel,net_iface,mac;
    int cores=0,threads=0; long ram_gb=0; bool loaded=false;
};
struct SysMeta { std::string hostname,cpu_model; int cores=0; double uptime=0; };

struct AppState {
    SysMeta meta; HWInfo hw;
    MemTypeInfo mem_type;
    CPUStats cpu_prev,cpu_curr;
    std::vector<double> cpu_pct;
    std::deque<double> cpu_hist;
    std::vector<Sensor> sensors;
    MemStats mem;
    std::vector<NetIface> net_prev,net_curr;
    std::map<std::string,std::deque<double>> rx_hist,tx_hist;
    std::vector<DiskInfo> disks;
    std::map<std::string,DiskIO> disk_io_prev,disk_io_curr;
    BattInfo batt;
    std::vector<ProcInfo> procs;
};

//  UTILITY
static std::string trim(const std::string& s){
    auto a=s.find_first_not_of(" \t\r\n");
    if(a==std::string::npos) return "";
    return s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
static std::string fmtBytes(unsigned long long b){
    std::ostringstream o;
    if(b<1024)            o<<b<<"B";
    else if(b<1048576)    o<<std::fixed<<std::setprecision(1)<<b/1024.0<<"K";
    else if(b<1073741824) o<<std::fixed<<std::setprecision(1)<<b/1048576.0<<"M";
    else                  o<<std::fixed<<std::setprecision(2)<<b/1073741824.0<<"G";
    return o.str();
}
static std::string fmtRate(double bps){
    if(bps<0) bps=0;
    std::ostringstream o;
    if(bps<1024)       o<<(int)bps<<"B/s";
    else if(bps<1048576) o<<std::fixed<<std::setprecision(1)<<bps/1024<<"K/s";
    else               o<<std::fixed<<std::setprecision(2)<<bps/1048576<<"M/s";
    return o.str();
}
static std::string fmtUptime(double s){
    int si=(int)s,d=si/86400; si%=86400; int h=si/3600; si%=3600; int m=si/60;
    std::ostringstream o;
    if(d) o<<d<<"d ";
    o<<std::setfill('0')<<std::setw(2)<<h<<":"<<std::setw(2)<<m;
    return o.str();
}
static std::string padR(std::string s,int w){
    if((int)s.size()<w) s.append(w-s.size(),' ');
    else if((int)s.size()>w) s=s.substr(0,w);
    return s;
}
static std::string padL(std::string s,int w){
    if((int)s.size()<w) s=std::string(w-s.size(),' ')+s;
    else if((int)s.size()>w) s=s.substr(s.size()-w);
    return s;
}
static int usageColor(double p){
    if(p<55) return C_GREEN;
    if(p<82) return C_BLUE;
    return C_RED;
}
static int tempColor(float t){
    if(t<55) return C_GREEN;
    if(t<75) return C_BLUE;
    return C_RED;
}

//  RAM TYPE DETECTION
static MemTypeInfo readMemType(){
    MemTypeInfo info;
    info.type = "Unknown";
    info.speed = "Unknown";
    info.form = "Unknown";
    
    // Try reading from DMI tables
    std::ifstream dmi_mem("/sys/class/dmi/id/product_name");
    
    // Try parsing dmidecode output if available
    FILE* pipe = popen("dmidecode -t memory 2>/dev/null | grep -E 'Type:|Speed:' | head -4", "r");
    if(pipe){
        char buffer[256];
        std::string output;
        while(fgets(buffer, sizeof(buffer), pipe)){
            output += buffer;
        }
        pclose(pipe);
        
        // Parse Type
        size_t type_pos = output.find("Type:");
        if(type_pos != std::string::npos){
            std::string line = output.substr(type_pos);
            size_t end = line.find('\n');
            if(end != std::string::npos) line = line.substr(0, end);
            
            if(line.find("DDR4") != std::string::npos) info.type = "DDR4";
            else if(line.find("DDR3") != std::string::npos) info.type = "DDR3";
            else if(line.find("DDR5") != std::string::npos) info.type = "DDR5";
            else if(line.find("DDR2") != std::string::npos) info.type = "DDR2";
            else if(line.find("DDR") != std::string::npos) info.type = "DDR";
        }
        
        // Parse Speed
        size_t speed_pos = output.find("Speed:");
        if(speed_pos != std::string::npos){
            std::string line = output.substr(speed_pos);
            size_t end = line.find('\n');
            if(end != std::string::npos) line = line.substr(0, end);
            
            size_t num_pos = line.find_first_of("0123456789");
            if(num_pos != std::string::npos){
                info.speed = line.substr(num_pos);
                // Clean up
                size_t mt_pos = info.speed.find("MT");
                if(mt_pos == std::string::npos){
                    size_t mhz_pos = info.speed.find("MHz");
                    if(mhz_pos != std::string::npos){
                        info.speed = info.speed.substr(0, mhz_pos) + "MT/s";
                    }
                }
            }
        }
    }
    
    // Fallback: try to read from /sys/devices/system/memory/
    if(info.type == "Unknown"){
        std::ifstream meminfo("/proc/meminfo");
        std::string line;
        while(std::getline(meminfo, line)){
            if(line.find("MemTotal") != std::string::npos){
                // Can't determine type from meminfo, but we have total
                break;
            }
        }
    }
    
    // Detect form factor from system type
    std::ifstream chassis("/sys/class/dmi/id/chassis_type");
    if(chassis){
        int type; chassis >> type;
        if(type == 8 || type == 9 || type == 10) info.form = "SODIMM";  // Laptop
        else if(type == 3 || type == 4) info.form = "DIMM";  // Desktop
    }
    
    info.detected = (info.type != "Unknown");
    return info;
}

//  READERS
static CPUStats readCPU(){
    CPUStats s; std::ifstream f("/proc/stat"); std::string line;
    while(std::getline(f,line)){
        if(line.size()<3||line.substr(0,3)!="cpu") break;
        std::istringstream ss(line); std::string tag; CPUCore c;
        ss>>tag>>c.user>>c.nice>>c.system>>c.idle>>c.iowait>>c.irq>>c.softirq>>c.steal;
        s.cores.push_back(c);
    }
    for(int i=0;i<16;i++){
        std::string p="/sys/devices/system/cpu/cpu"+std::to_string(i)+"/cpufreq/scaling_cur_freq";
        std::ifstream ff(p);
        if(ff){long v;ff>>v;s.freq_mhz=v/1000.0;break;}
    }
    return s;
}
static MemStats readMem(){
    MemStats m; std::map<std::string,long> kv;
    std::ifstream f("/proc/meminfo"); std::string line;
    while(std::getline(f,line)){
        std::istringstream ss(line); std::string k; long v; ss>>k>>v;
        if(!k.empty() && k.back()==':') k.pop_back();
        kv[k]=v;
    }
    m.total=kv.count("MemTotal")?kv["MemTotal"]:0;
    m.free=kv.count("MemFree")?kv["MemFree"]:0;
    m.available=kv.count("MemAvailable")?kv["MemAvailable"]:0;
    m.buffers=kv.count("Buffers")?kv["Buffers"]:0;
    m.cached=kv.count("Cached")?kv["Cached"]:0;
    m.sreclaimable=kv.count("SReclaimable")?kv["SReclaimable"]:0;
    m.swap_total=kv.count("SwapTotal")?kv["SwapTotal"]:0;
    m.swap_free=kv.count("SwapFree")?kv["SwapFree"]:0;
    return m;
}
static std::vector<Sensor> readSensors(){
    std::vector<Sensor> cpu_s, other_s;
    for(int h=0; h<10; h++){
        std::string base="/sys/class/hwmon/hwmon"+std::to_string(h)+"/";
        std::ifstream nf(base+"name"); if(!nf) continue;
        std::string chip; nf>>chip;
        std::string type="SYS"; bool is_cpu=false, is_bat=false;
        if(chip.find("coretemp")!=std::string::npos || chip.find("k10temp")!=std::string::npos){
            type="CPU"; is_cpu=true;
        } else if(chip.find("nvidia")!=std::string::npos || chip.find("nouveau")!=std::string::npos){
            type="GPU";
        } else if(chip.find("acpi")!=std::string::npos || chip.find("BAT")!=std::string::npos){
            type="BAT"; is_bat=true;
        } else if(chip.find("pch")!=std::string::npos){
            type="MB";
        }
        for(int j=1; j<=8 && (int)(cpu_s.size()+other_s.size())<6; j++){
            std::string tp=base+"temp"+std::to_string(j)+"_input";
            std::ifstream tf(tp); if(!tf) continue;
            int milli; tf>>milli;
            float tc=milli/1000.0f;
            if(tc<25 || tc>100) continue;
            std::string lbl=type;
            std::string lp=base+"temp"+std::to_string(j)+"_label";
            std::ifstream lf(lp);
            if(lf){ std::getline(lf,lbl); lbl=trim(lbl); if(lbl.size()>10)lbl=lbl.substr(0,10); }
            else if(is_cpu) lbl="Core"+std::to_string(j-1);
            if(is_cpu) cpu_s.push_back({lbl,type,tc});
            else if(!is_bat) other_s.push_back({lbl,type,tc});
        }
    }
    if(cpu_s.empty()){
        for(int i=0; i<8; i++){
            std::string base="/sys/class/thermal/thermal_zone"+std::to_string(i);
            std::ifstream tf(base+"/temp"); if(!tf) continue;
            int m; tf>>m; float tc=m/1000.0f;
            if(tc<25 || tc>100) continue;
            std::string type="ZONE";
            std::ifstream tyf(base+"/type");
            if(tyf){ std::getline(tyf,type); type=trim(type); }
            bool is_cpu=(type.find("cpu")!=std::string::npos || type.find("x86")!=std::string::npos || type.find("package")!=std::string::npos);
            if(is_cpu) cpu_s.push_back({type.substr(0,10),"CPU",tc});
            else other_s.push_back({type.substr(0,10),"SYS",tc});
        }
    }
    std::vector<Sensor> out;
    for(auto& s:cpu_s) { if((int)out.size()>=4) break; out.push_back(s); }
    for(auto& s:other_s) { if((int)out.size()>=4) break; out.push_back(s); }
    return out;
}
static std::vector<NetIface> readNet(){
    std::vector<NetIface> v;
    std::vector<NetIface> wifi_ifaces;
    std::vector<NetIface> other_ifaces;
    
    std::ifstream f("/proc/net/dev"); std::string line;
    std::getline(f,line); std::getline(f,line);
    
    while(std::getline(f,line)){
        std::istringstream ss(line); std::string name; ss>>name;
        if(!name.empty() && name.back()==':') name.pop_back();
        if(name=="lo") continue;
        
        NetIface n; n.name=name;
        long long dummy;
        ss>>n.rx_bytes>>dummy>>dummy>>dummy>>dummy>>dummy>>dummy>>dummy>>n.tx_bytes>>dummy;
        
        if(name.find("w")==0){
            n.primary=true;
            wifi_ifaces.push_back(n);
        }else{
            other_ifaces.push_back(n);
        }
    }
    
    v = wifi_ifaces;
    v.insert(v.end(), other_ifaces.begin(), other_ifaces.end());
    
    if(wifi_ifaces.empty() && !v.empty()){
        v[0].primary=true;
    }
    
    return v;
}
static std::vector<DiskInfo> readDisks(){
    std::vector<DiskInfo> disks;
    std::ifstream f("/proc/mounts"); std::string line;
    std::vector<std::string> seen;
    static const std::vector<std::string> skip_fs={"tmpfs","devtmpfs","sysfs","proc","cgroup","overlay","squashfs"};
    while(std::getline(f,line) && (int)disks.size()<4){  // Reduced from 6 to 4
        std::istringstream ss(line); std::string dev,mount,fs;
        ss>>dev>>mount>>fs;
        bool skip=false;
        for(auto& s:skip_fs) if(fs==s) skip=true;
        if(skip) continue;
        if(mount.substr(0,4)=="/sys" || mount.substr(0,5)=="/proc" || 
           mount.substr(0,4)=="/dev" || mount.substr(0,4)=="/run") continue;
        if(std::find(seen.begin(),seen.end(),mount)!=seen.end()) continue;
        seen.push_back(mount);
        struct statvfs sv;
        if(statvfs(mount.c_str(),&sv)!=0) continue;
        DiskInfo d; d.device=dev; d.mount=mount;
        d.total=(unsigned long long)sv.f_blocks * sv.f_frsize;
        d.used=d.total - (unsigned long long)sv.f_bfree * sv.f_frsize;
        if(d.total > 1024*1024) disks.push_back(d);
    }
    return disks;
}
static std::map<std::string,DiskIO> readDiskIO(){
    std::map<std::string,DiskIO> m;
    std::ifstream f("/proc/diskstats"); std::string line;
    while(std::getline(f,line)){
        std::istringstream ss(line); int maj,min; std::string name;
        ss>>maj>>min>>name;
        long long dummy,rs,ws;
        for(int i=0;i<3;i++) ss>>dummy;
        ss>>rs;
        for(int i=0;i<3;i++) ss>>dummy;
        ss>>ws;
        if(name.find("sd")==0 || name.find("nvme")==0 || name.find("mmc")==0)
            m[name]={rs*512, ws*512};
    }
    return m;
}
static BattInfo readBatt(){
    BattInfo b;
    for(const char* p:{"BAT0","BAT1","battery"}){
        std::string base="/sys/class/power_supply/"+std::string(p)+"/";
        std::ifstream cf(base+"capacity"); if(!cf) continue;
        b.present=true; cf>>b.pct;
        std::string st; std::ifstream sf(base+"status"); if(sf) sf>>st;
        b.charging=(st=="Charging"); b.full=(st=="Full");
        break;
    }
    return b;
}
static SysMeta readMeta(){
    SysMeta m;
    std::ifstream hf("/etc/hostname"); std::getline(hf,m.hostname);
    if(m.hostname.empty()) m.hostname="localhost";
    std::ifstream cf("/proc/cpuinfo"); std::string line;
    while(std::getline(cf,line)){
        if(line.find("model name")!=std::string::npos){
            auto pos=line.find(':');
            if(pos!=std::string::npos){ m.cpu_model=trim(line.substr(pos+2)); break; }
        }
    }
    if((int)m.cpu_model.size()>40) m.cpu_model=m.cpu_model.substr(0,38)+"..";
    m.cores=sysconf(_SC_NPROCESSORS_ONLN);
    std::ifstream uf("/proc/uptime"); uf>>m.uptime;
    return m;
}
static HWInfo readHW(const SysMeta& meta){
    HWInfo hw; hw.loaded=true;
    hw.cpu=meta.cpu_model; hw.cores=meta.cores;
    std::ifstream df("/sys/class/dmi/id/product_name");
    if(df) std::getline(df,hw.model);
    hw.model=trim(hw.model);
    if(hw.model.empty()) hw.model="Unknown";
    std::ifstream cf("/proc/cpuinfo"); std::string line; int thr=0;
    while(std::getline(cf,line)){
        if(line.find("processor")!=std::string::npos) thr++;
        if(line.find("cpu cores")!=std::string::npos){
            auto p=line.find(':');
            if(p!=std::string::npos) hw.cores=std::stoi(trim(line.substr(p+1)));
        }
    }
    hw.threads=thr; if(hw.cores==0) hw.cores=thr;
    std::ifstream mf("/proc/meminfo");
    while(std::getline(mf,line)){
        if(line.find("MemTotal")!=std::string::npos){
            auto p=line.find(':');
            if(p!=std::string::npos) hw.ram_gb=std::stol(trim(line.substr(p+1)))/1024/1024;
        }
    }
    std::ifstream kf("/proc/version"); std::getline(kf,line);
    auto p1=line.find("version ");
    if(p1!=std::string::npos){
        line=line.substr(p1+8);
        auto p2=line.find(' ');
        if(p2!=std::string::npos) hw.kernel=line.substr(0,p2);
    }
    hw.gpu=(access("/dev/nvidia0",F_OK)==0) ? "NVIDIA GPU" : "Integrated Graphics";
    DIR* dir=opendir("/sys/class/net");
    if(dir){
        struct dirent* e;
        while((e=readdir(dir))){
            std::string n=e->d_name;
            if(n=="lo") continue;
            if(n=="wlo1" || n.find("wl")==0){ hw.net_iface=n; break; }
            if(hw.net_iface.empty()) hw.net_iface=n;
        }
        closedir(dir);
    }
    if(!hw.net_iface.empty()){
        std::ifstream macf("/sys/class/net/"+hw.net_iface+"/address");
        if(macf) std::getline(macf,hw.mac);
        bool wifi=false;
        std::ifstream wf("/proc/net/wireless");
        while(std::getline(wf,line))
            if(line.find(hw.net_iface)!=std::string::npos){ wifi=true; break; }
        hw.net_iface += (wifi ? " (WiFi)" : " (Ethernet)");
    }
    if(hw.mac.empty()) hw.mac="N/A";
    return hw;
}

//  PROCESS SIGNAL FUNCTIONS
struct SignalOption {
    int num;
    const char* name;
    const char* desc;
};

static const SignalOption SIGNALS[] = {
    {15, "SIGTERM", "Terminate (graceful)"},
    {9,  "SIGKILL", "Kill (force)"},
    {2,  "SIGINT",  "Interrupt (Ctrl+C)"},
    {1,  "SIGHUP",  "Hangup (reload)"},
    {18, "SIGCONT", "Continue"},
    {19, "SIGSTOP", "Stop (pause)"},
};
static const int NUM_SIGNALS = 6;

static int sendSignalToProcess(int pid, int sig){
    if(kill(pid, sig) == 0){
        return 0;  // Success
    }
    return -1;  // Failed
}

//  ENHANCED SIGNAL SEND UI
static int g_signal_selected = 0;  // Which signal is highlighted

static void drawSignalUI(const AppState& st, int rows, int cols){
    if(g_cfg.proc_selected >= (int)st.procs.size()) return;
    const auto& proc = st.procs[g_cfg.proc_selected];
    
    // UI Dimensions
    int ui_w = cols - 4;
    int ui_h = rows - 6;
    int ui_y = 2;
    int ui_x = 2;
    int left_w = ui_w / 3;  // Left: Signal list (1/3)
    int right_w = ui_w - left_w - 1;  // Right: Process info (2/3)
    
    // Draw outer border
    attron(COLOR_PAIR(C_WARNING) | A_BOLD);
    mvaddstr(ui_y, ui_x, "╭");
    mvaddstr(ui_y, ui_x+ui_w-1, "╮");
    mvaddstr(ui_y+ui_h-1, ui_x, "╰");
    mvaddstr(ui_y+ui_h-1, ui_x+ui_w-1, "╯");
    for(int i=ui_x+1; i<ui_x+ui_w-1; i++){
        mvaddstr(ui_y, i, "─");
        mvaddstr(ui_y+ui_h-1, i, "─");
    }
    for(int i=ui_y+1; i<ui_y+ui_h-1; i++){
        mvaddstr(i, ui_x, "│");
        mvaddstr(i, ui_x+ui_w-1, "│");
    }
    // Vertical divider
    for(int i=ui_y+1; i<ui_y+ui_h-1; i++){
        mvaddstr(i, ui_x+left_w, "│");
    }
    attroff(COLOR_PAIR(C_WARNING) | A_BOLD);
    
    // Title
    attron(COLOR_PAIR(C_WARNING) | A_BOLD);
    mvaddstr(ui_y, ui_x+2, " SEND SIGNAL TO PROCESS ");
    attroff(COLOR_PAIR(C_WARNING) | A_BOLD);
    
    // LEFT SIDE: Signal List
    attron(COLOR_PAIR(C_TITLE) | A_BOLD);
    mvaddstr(ui_y+2, ui_x+2, "SIGNALS");
    attroff(COLOR_PAIR(C_TITLE) | A_BOLD);
    
    for(int i=0; i<NUM_SIGNALS && i<ui_h-5; i++){
        int y = ui_y + 4 + i;
        bool selected = (i == g_signal_selected);
        
        if(selected){
            attron(COLOR_PAIR(C_SELECTED));
            mvaddstr(y, ui_x+1, "▸ ");
            attroff(COLOR_PAIR(C_SELECTED));
        }else{
            mvaddstr(y, ui_x+1, "  ");
        }
        
        attron(selected ? (COLOR_PAIR(C_SELECTED)|A_BOLD) : COLOR_PAIR(C_DIM));
        std::ostringstream opt;
        opt << SIGNALS[i].name << " (" << SIGNALS[i].num << ")";
        mvaddstr(y, ui_x+3, opt.str().c_str());
        attroff(selected ? (COLOR_PAIR(C_SELECTED)|A_BOLD) : COLOR_PAIR(C_DIM));
        
        // Description
        attron(COLOR_PAIR(C_DIM));
        mvaddstr(y, ui_x+left_w-18, SIGNALS[i].desc);
        attroff(COLOR_PAIR(C_DIM));
    }
    
    // RIGHT SIDE: Process Information
    attron(COLOR_PAIR(C_TITLE) | A_BOLD);
    mvaddstr(ui_y+2, ui_x+left_w+2, "PROCESS INFO");
    attroff(COLOR_PAIR(C_TITLE) | A_BOLD);
    
    int info_y = ui_y + 4;
    int info_x = ui_x + left_w + 2;
    
    // Process name
    attron(COLOR_PAIR(C_GREEN) | A_BOLD);
    mvaddstr(info_y, info_x, "Name:");
    attroff(COLOR_PAIR(C_GREEN) | A_BOLD);
    attron(COLOR_PAIR(C_DEFAULT));
    mvaddstr(info_y, info_x+8, proc.name.c_str());
    attroff(COLOR_PAIR(C_DEFAULT));
    info_y++;
    
    // PID
    attron(COLOR_PAIR(C_GREEN) | A_BOLD);
    mvaddstr(info_y, info_x, "PID:");
    attroff(COLOR_PAIR(C_GREEN) | A_BOLD);
    std::ostringstream pid;
    pid << proc.pid;
    attron(COLOR_PAIR(C_DEFAULT));
    mvaddstr(info_y, info_x+8, pid.str().c_str());
    attroff(COLOR_PAIR(C_DEFAULT));
    info_y++;
    
    // State
    attron(COLOR_PAIR(C_GREEN) | A_BOLD);
    mvaddstr(info_y, info_x, "State:");
    attroff(COLOR_PAIR(C_GREEN) | A_BOLD);
    std::string state;
    switch(proc.state){
        case 'R': state = "Running"; break;
        case 'S': state = "Sleeping"; break;
        case 'D': state = "Disk Wait"; break;
        case 'Z': state = "Zombie"; break;
        case 'T': state = "Stopped"; break;
        default: state = "Unknown"; break;
    }
    attron(COLOR_PAIR(C_DEFAULT));
    mvaddstr(info_y, info_x+8, state.c_str());
    attroff(COLOR_PAIR(C_DEFAULT));
    info_y++;
    
    // CPU%
    attron(COLOR_PAIR(C_GREEN) | A_BOLD);
    mvaddstr(info_y, info_x, "CPU%:");
    attroff(COLOR_PAIR(C_GREEN) | A_BOLD);
    std::ostringstream cpu;
    cpu << std::fixed << std::setprecision(1) << proc.cpu_pct << "%";
    int cpu_c = usageColor(proc.cpu_pct);
    attron(COLOR_PAIR(cpu_c));
    mvaddstr(info_y, info_x+8, cpu.str().c_str());
    attroff(COLOR_PAIR(cpu_c));
    info_y++;
    
    // Memory
    attron(COLOR_PAIR(C_GREEN) | A_BOLD);
    mvaddstr(info_y, info_x, "Memory:");
    attroff(COLOR_PAIR(C_GREEN) | A_BOLD);
    std::ostringstream mem;
    mem << proc.mem_kb/1024 << " MB (" << std::fixed << std::setprecision(1) 
        << (st.mem.total>0 ? 100.0*proc.mem_kb/st.mem.total : 0) << "%)";
    attron(COLOR_PAIR(C_DEFAULT));
    mvaddstr(info_y, info_x+8, mem.str().c_str());
    attroff(COLOR_PAIR(C_DEFAULT));
    info_y++;
    
    // Divider
    info_y++;
    attron(COLOR_PAIR(C_BORDER));
    for(int i=info_x; i<ui_x+ui_w-2; i++) mvaddstr(info_y, i, "─");
    attroff(COLOR_PAIR(C_BORDER));
    info_y++;
    
    // Additional info
    attron(COLOR_PAIR(C_DIM));
    mvaddstr(info_y, info_x, "Press ENTER to send selected signal");
    info_y++;
    mvaddstr(info_y, info_x, "Press Q or ESC to cancel");
    info_y++;
    mvaddstr(info_y, info_x, "Use ↑↓ to select signal");
    attroff(COLOR_PAIR(C_DIM));
    
    // Bottom action bar
    attron(COLOR_PAIR(C_WARNING));
    mvaddstr(ui_y+ui_h-2, ui_x+1, "╴");
    for(int i=ui_x+2; i<ui_x+ui_w-2; i++) mvaddstr(ui_y+ui_h-2, i, "─");
    mvaddstr(ui_y+ui_h-2, ui_x+ui_w-3, "╶");
    attroff(COLOR_PAIR(C_WARNING));
    
    attron(COLOR_PAIR(C_SELECTED) | A_BOLD);
    std::string action = " ENTER:Send  Q:Cancel ";
    mvaddstr(ui_y+ui_h-2, ui_x+(ui_w-(int)action.size())/2, action.c_str());
    attroff(COLOR_PAIR(C_SELECTED) | A_BOLD);
}

static int handleSignalUI(AppState& st){
    if(g_cfg.proc_selected >= (int)st.procs.size()) return 0;
    const auto& proc = st.procs[g_cfg.proc_selected];
    
    // Switch to blocking input for signal UI
    nocbreak();
    cbreak();
    echo();
    keypad(stdscr, TRUE);
    
    int sig = 0;
    bool done = false;
    
    while(!done){
        drawSignalUI(st, LINES, COLS);
        refresh();
        
        int ch = getch();
        
        switch(ch){
            case KEY_UP:
                g_signal_selected--;
                if(g_signal_selected < 0) g_signal_selected = NUM_SIGNALS - 1;
                break;
            case KEY_DOWN:
                g_signal_selected++;
                if(g_signal_selected >= NUM_SIGNALS) g_signal_selected = 0;
                break;
            case '\n': case KEY_ENTER: case 13:
                sig = SIGNALS[g_signal_selected].num;
                done = true;
                break;
            case 'q': case 'Q': case 27:  // 27 = ESC
                sig = 0;  // Cancel
                done = true;
                break;
        }
    }
    
    // Restore normal input mode
    noecho();
    nodelay(stdscr, TRUE);
    
    if(sig > 0){
        int result = sendSignalToProcess(proc.pid, sig);
        
        // Show result message
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        
        attron(COLOR_PAIR(result == 0 ? C_GREEN : C_RED) | A_BOLD);
        std::string msg = result == 0 ? 
            "✓ Signal " + std::to_string(sig) + " sent to PID " + std::to_string(proc.pid) : 
            "✗ Failed (permission denied or process ended)";
        mvaddstr(rows-2, 2, msg.c_str());
        attroff(COLOR_PAIR(result == 0 ? C_GREEN : C_RED) | A_BOLD);
        refresh();
        usleep(800000);  // Show message for 800ms
    }
    
    g_cfg.signal_mode = false;
    g_signal_selected = 0;
    return sig;
}

static std::map<int,long long> g_prev_proc_t;
static std::vector<ProcInfo> readProcs(long long cpu_delta){
    std::vector<ProcInfo> procs;
    DIR* dir=opendir("/proc"); if(!dir) return procs;
    struct dirent* e;
    while((e=readdir(dir))){
        bool is_pid=true;
        for(char* p=e->d_name; *p; p++) if(*p<'0'||*p>'9'){is_pid=false;break;}
        if(!is_pid) continue;
        int pid=atoi(e->d_name);
        std::string sp="/proc/"+std::string(e->d_name)+"/stat";
        std::ifstream sf(sp); if(!sf) continue;
        std::string line; std::getline(sf,line);
        auto p1=line.find('('), p2=line.rfind(')');
        if(p1==std::string::npos || p2==std::string::npos) continue;
        ProcInfo pi; pi.pid=pid; pi.name=line.substr(p1+1,p2-p1-1);
        std::istringstream ss(line.substr(p2+2));
        char st; int di; long long dull;
        ss>>st>>di>>di>>di>>di>>di>>dull>>dull>>dull>>dull>>dull>>pi.cpu_ticks;
        pi.state=st;
        std::string ms="/proc/"+std::string(e->d_name)+"/status";
        std::ifstream msf(ms); std::string ml;
        while(std::getline(msf,ml)){
            if(ml.find("VmRSS:")==0){
                std::istringstream mss(ml); std::string k; mss>>k>>pi.mem_kb;
                break;
            }
            if(ml.find("Uid:")==0){
                std::istringstream mss(ml); std::string k; int uid;
                mss>>k>>uid;
                // Could lookup username from /etc/passwd
                pi.user = std::to_string(uid);
            }
        }
        if(g_prev_proc_t.count(pid) && cpu_delta>0)
            pi.cpu_pct=100.0*(pi.cpu_ticks-g_prev_proc_t[pid])/cpu_delta;
        g_prev_proc_t[pid]=pi.cpu_ticks;
        procs.push_back(pi);
    }
    closedir(dir);
    std::sort(procs.begin(),procs.end(),[](const ProcInfo&a,const ProcInfo&b){
        if(std::abs(a.cpu_pct-b.cpu_pct)>0.05) return a.cpu_pct>b.cpu_pct;
        return a.mem_kb>b.mem_kb;
    });
    if((int)procs.size()>30) procs.resize(30);
    return procs;
}

//  STATE UPDATE
static void updateState(AppState& st){
    st.cpu_prev=st.cpu_curr; st.net_prev=st.net_curr; st.disk_io_prev=st.disk_io_curr;
    st.cpu_curr=readCPU(); st.mem=readMem(); st.sensors=readSensors();
    st.net_curr=readNet(); st.disks=readDisks();
    st.disk_io_curr=readDiskIO(); st.batt=readBatt();
    st.meta.uptime=readMeta().uptime;
    
    // Read RAM type once (doesn't change)
    if(!st.mem_type.detected){
        st.mem_type = readMemType();
    }
    
    if(!st.cpu_prev.cores.empty() && !st.cpu_curr.cores.empty()){
        int n=std::min(st.cpu_prev.cores.size(),st.cpu_curr.cores.size());
        st.cpu_pct.resize(n);
        for(int i=0;i<n;i++){
            long long tot=st.cpu_curr.cores[i].total()-st.cpu_prev.cores[i].total();
            long long idle=(st.cpu_curr.cores[i].idle+st.cpu_curr.cores[i].iowait)
                          -(st.cpu_prev.cores[i].idle+st.cpu_prev.cores[i].iowait);
            st.cpu_pct[i]=tot>0 ? 100.0*(tot-idle)/tot : 0.0;
        }
        if(!st.cpu_pct.empty()){
            st.cpu_hist.push_back(st.cpu_pct[0]);
            if((int)st.cpu_hist.size()>60) st.cpu_hist.pop_front();
        }
    }
    double dt=g_cfg.update_ms/1000.0;
    for(auto& c:st.net_curr){
        for(auto& p:st.net_prev){
            if(c.name==p.name){
                c.rx_rate=(c.rx_bytes-p.rx_bytes)/dt;
                c.tx_rate=(c.tx_bytes-p.tx_bytes)/dt;
                auto& rxh=st.rx_hist[c.name];
                rxh.push_back(c.rx_rate);
                if((int)rxh.size()>40) rxh.pop_front();
                auto& txh=st.tx_hist[c.name];
                txh.push_back(c.tx_rate);
                if((int)txh.size()>40) txh.pop_front();
                break;
            }
        }
    }
    for(auto& d:st.disks){
        std::string dev=d.device;
        if(dev.find("/dev/")==0) dev=dev.substr(5);
        while(!dev.empty() && dev.back()>='0' && dev.back()<='9' && dev.find("nvme")==std::string::npos)
            dev.pop_back();
        auto it=st.disk_io_curr.find(dev), prev=st.disk_io_prev.find(dev);
        if(it!=st.disk_io_curr.end() && prev!=st.disk_io_prev.end()){
            d.read_rate=(it->second.rb - prev->second.rb)/dt;
            d.write_rate=(it->second.wb - prev->second.wb)/dt;
        }
    }
    long long cpu_delta=0;
    if(!st.cpu_prev.cores.empty() && !st.cpu_curr.cores.empty())
        cpu_delta=st.cpu_curr.cores[0].total()-st.cpu_prev.cores[0].total();
    if(g_cfg.proc && cpu_delta>0) st.procs=readProcs(cpu_delta);
}

//  DRAWING
static const char* SPARK[]={" ","▁","▂","▃","▄","▅","▆","▇","█"};

static void drawGradientBar(int y,int x,int w,double pct){
    if(w<3) return;
    pct=std::max(0.0,std::min(100.0,pct));
    int filled=(int)(pct/100.0*(w-6)+0.5);
    filled=std::min(filled,w-6);
    for(int i=0;i<filled;i++){
        int cp;
        double pos=(double)i/(w-6);
        if(pos<0.45) cp=C_GRAD_G;
        else if(pos<0.75) cp=C_GRAD_B;
        else cp=C_GRAD_R;
        attron(COLOR_PAIR(cp)|A_BOLD);
        mvaddstr(y,x+i,"█");
        attroff(COLOR_PAIR(cp)|A_BOLD);
    }
    attron(COLOR_PAIR(C_BAR_EMPTY)|A_DIM);
    for(int i=filled;i<w-6;i++) mvaddstr(y,x+i,"░");
    attroff(COLOR_PAIR(C_BAR_EMPTY)|A_DIM);
    std::ostringstream ps; ps<<std::fixed<<std::setprecision(0)<<pct<<"%";
    std::string pstr=padL(ps.str(),5);
    int lc=usageColor(pct);
    attron(COLOR_PAIR(lc)|A_BOLD);
    mvaddstr(y,x+w-6,pstr.c_str());
    attroff(COLOR_PAIR(lc)|A_BOLD);
}

static void drawBarSimple(int y,int x,int w,double pct,int cp){
    if(w<1) return;
    pct=std::max(0.0,std::min(100.0,pct));
    int filled=(int)(pct/100.0*w+0.5);
    filled=std::min(filled,w);
    for(int i=0;i<filled;i++){
        attron(COLOR_PAIR(cp)|A_BOLD);
        mvaddstr(y,x+i,"█");
        attroff(COLOR_PAIR(cp)|A_BOLD);
    }
    attron(COLOR_PAIR(C_BAR_EMPTY)|A_DIM);
    for(int i=filled;i<w;i++) mvaddstr(y,x+i,"░");
    attroff(COLOR_PAIR(C_BAR_EMPTY)|A_DIM);
}

static void drawSpark(int y,int x,int w,const std::deque<double>& hist,int cp){
    if(w<1 || hist.empty()) return;
    double mx=*std::max_element(hist.begin(),hist.end());
    if(mx<1) mx=1;
    attron(COLOR_PAIR(cp));
    for(int i=0;i<w;i++){
        int idx=(int)hist.size()-w+i;
        if(idx<0){ mvaddch(y,x+i,' '); continue; }
        int s=(int)(hist[idx]/mx*8.0+0.5);
        s=std::max(0,std::min(8,s));
        mvaddstr(y,x+i,SPARK[s]);
    }
    attroff(COLOR_PAIR(cp));
}

static void drawPanel(int y,int x,int h,int w,const std::string& title){
    if(h<3 || w<6) return;
    attron(COLOR_PAIR(C_BORDER));
    mvaddstr(y,x,"╭");
    for(int i=x+1;i<x+w-1;i++) mvaddstr(y,i,"─");
    mvaddstr(y,x+w-1,"╮");
    for(int i=y+1;i<y+h-1;i++){
        mvaddstr(i,x,"│");
        mvaddstr(i,x+w-1,"│");
    }
    mvaddstr(y+h-1,x,"╰");
    for(int i=x+1;i<x+w-1;i++) mvaddstr(y+h-1,i,"─");
    mvaddstr(y+h-1,x+w-1,"╯");
    attroff(COLOR_PAIR(C_BORDER));
    if(!title.empty() && w>8){
        std::string t=" "+title+" ";
        if((int)t.size()>w-6) t=t.substr(0,w-6);
        attron(COLOR_PAIR(C_TITLE)|A_BOLD);
        mvaddstr(y,x+2,t.c_str());
        attroff(COLOR_PAIR(C_TITLE)|A_BOLD);
    }
}

static void divline(int y,int x,int w){
    attron(COLOR_PAIR(C_BORDER));
    mvaddstr(y,x,"├");
    for(int i=x+1;i<x+w-1;i++) mvaddstr(y,i,"─");
    mvaddstr(y,x+w-1,"┤");
    attroff(COLOR_PAIR(C_BORDER));
}


//  TOP / BOTTOM BARS
static void drawTopBar(const AppState& st,int cols){
    attron(COLOR_PAIR(C_HBAR)|A_BOLD);
    for(int i=0;i<cols;i++) mvaddch(0,i,' ');
    mvaddstr(0,2,"◈ TTOP");
    attroff(COLOR_PAIR(C_HBAR)|A_BOLD);
    attron(COLOR_PAIR(C_HBAR));
    mvaddstr(0,10,("│ "+st.meta.hostname).c_str());
    attroff(COLOR_PAIR(C_HBAR));
    std::ostringstream fps; fps<<std::fixed<<std::setprecision(0)<<g_fps.fps<<" FPS";
    attron(COLOR_PAIR(C_GREEN)|A_BOLD);
    mvaddstr(0,cols/2-6,fps.str().c_str());
    attroff(COLOR_PAIR(C_GREEN)|A_BOLD);
    time_t t=time(nullptr); char tb[10];
    strftime(tb,sizeof(tb),"%H:%M:%S",localtime(&t));
    std::string right="up "+fmtUptime(st.meta.uptime)+" │ "+std::string(tb);
    attron(COLOR_PAIR(C_HBAR));
    mvaddstr(0,cols-(int)right.size()-2,right.c_str());
    attroff(COLOR_PAIR(C_HBAR));
    if(st.batt.present){
        std::string icon=st.batt.charging?"⚡":st.batt.full?"✓":"▼";
        std::string bs=" "+icon+" "+std::to_string(st.batt.pct)+"%";
        int bc=st.batt.charging?C_GREEN:(st.batt.pct<20?C_RED:C_YELLOW);
        attron(COLOR_PAIR(bc)|A_BOLD);
        mvaddstr(0,cols-(int)right.size()-2-(int)bs.size(),bs.c_str());
        attroff(COLOR_PAIR(bc)|A_BOLD);
    }
    attron(COLOR_PAIR(C_DEFAULT));
    for(int i=0;i<cols;i++) mvaddch(1,i,' ');
    attroff(COLOR_PAIR(C_DEFAULT));
    struct Tab{const char* key,*name; bool on;};
    Tab tabs[]={{"1","CPU",g_cfg.cpu},{"2","MEM",g_cfg.mem},{"3","NET",g_cfg.net},
                {"4","DSK",g_cfg.disk},{"5","HW",g_cfg.hw},{"6","PROC",g_cfg.proc}};
    int tx=2;
    for(auto& tab:tabs){
        std::string s=" "+std::string(tab.key)+":"+std::string(tab.name)+" ";
        if(tab.on){
            attron(COLOR_PAIR(C_SELECTED)|A_BOLD);
            mvaddstr(1,tx,s.c_str());
            attroff(COLOR_PAIR(C_SELECTED)|A_BOLD);
        }else{
            attron(COLOR_PAIR(C_DIM));
            mvaddstr(1,tx,s.c_str());
            attroff(COLOR_PAIR(C_DIM));
        }
        tx+=(int)s.size();
        attron(COLOR_PAIR(C_BORDER));
        mvaddstr(1,tx,"│");
        attroff(COLOR_PAIR(C_BORDER));
        tx++;
    }
    std::string tr=" T:"+std::string(THEME_NAMES[g_theme])+"  G:"+(g_cfg.graphs?"ON":"OFF");
    attron(COLOR_PAIR(C_TITLE)|A_BOLD);
    mvaddstr(1,cols-(int)tr.size()-2,tr.c_str());
    attroff(COLOR_PAIR(C_TITLE)|A_BOLD);
    attron(COLOR_PAIR(C_BORDER));
    for(int i=0;i<cols;i++) mvaddstr(2,i,"─");
    attroff(COLOR_PAIR(C_BORDER));
}

static void drawBottomBar(int rows,int cols){
    attron(COLOR_PAIR(C_HBAR));
    for(int i=0;i<cols;i++) mvaddch(rows-1,i,' ');
    struct K{const char* k,*d;};
    K keys[]={{"1-6","Panels"},{"K","Kill"},{"T","Theme"},{"G","Graphs"},{"F","Full"},{"+/-","Speed"},{"Q","Quit"}};
    int kx=2;
    for(auto& k:keys){
        attron(COLOR_PAIR(C_SELECTED)|A_BOLD);
        mvaddstr(rows-1,kx,k.k);
        attroff(COLOR_PAIR(C_SELECTED)|A_BOLD);
        attron(COLOR_PAIR(C_HBAR));
        std::string d=" "+std::string(k.d)+"  ";
        mvaddstr(rows-1,kx+(int)strlen(k.k),d.c_str());
        kx+=(int)strlen(k.k)+(int)d.size();
        attroff(COLOR_PAIR(C_HBAR));
        if(kx>=cols-20) break;
    }
    std::string spd=std::to_string(g_cfg.update_ms)+"ms";
    attron(COLOR_PAIR(C_SELECTED)|A_BOLD);
    mvaddstr(rows-1,cols-(int)spd.size()-2,spd.c_str());
    attroff(COLOR_PAIR(C_SELECTED)|A_BOLD);
    attroff(COLOR_PAIR(C_HBAR));
}

//  PANEL RENDERERS
static void pCPU(const AppState& st,int y,int x,int h,int w){
    if(h<6 || w<20) return;
    drawPanel(y,x,h,w,"CPU + SENSORS");
    int r=y+2;
    int inner_w=w-4;
    attron(COLOR_PAIR(C_DIM));
    std::string model=st.meta.cpu_model;
    if((int)model.size()>inner_w) model=model.substr(0,inner_w-2)+"..";
    mvaddstr(r,x+2,model.c_str());
    attroff(COLOR_PAIR(C_DIM));
    if(st.cpu_curr.freq_mhz>0){
        std::ostringstream fq; fq<<std::fixed<<std::setprecision(0)<<st.cpu_curr.freq_mhz<<"MHz";
        attron(COLOR_PAIR(C_CYAN)|A_BOLD);
        mvaddstr(r,x+w-2-(int)fq.str().size(),fq.str().c_str());
        attroff(COLOR_PAIR(C_CYAN)|A_BOLD);
    }
    r++;
    if(g_cfg.graphs && !st.cpu_hist.empty() && r<y+h-8){
        int gc=usageColor(st.cpu_pct.empty()?0:st.cpu_pct[0]);
        drawSpark(r,x+2,inner_w,st.cpu_hist,gc);
        r++;
    }
    int show_cores=std::min(4,(int)st.cpu_pct.size());
    for(int i=0;i<show_cores && r<y+h-5;i++){
        double pct=st.cpu_pct[i];
        std::string lbl=(i==0)?" ALL":" C"+std::to_string(i-1);
        attron(COLOR_PAIR(C_DIM));
        mvaddstr(r,x+2,padR(lbl,5).c_str());
        attroff(COLOR_PAIR(C_DIM));
        drawGradientBar(r,x+8,inner_w-8,pct);
        r++;
    }
    if(r<y+h-4){ divline(r,x,w); r++; }
    attron(COLOR_PAIR(C_TITLE)|A_BOLD);
    mvaddstr(r,x+2," SENSORS");
    attroff(COLOR_PAIR(C_TITLE)|A_BOLD);
    r++;
    int sensor_count=0;
    for(auto& s:st.sensors){
        if(r>=y+h-1 || sensor_count>=3) break;
        if(s.type=="BAT") continue;
        int tc=tempColor(s.temp);
        int lc=(s.type=="CPU"?C_RED:(s.type=="GPU"?C_PURPLE:C_CYAN));
        attron(COLOR_PAIR(lc));
        std::string lbl=" ["+s.type+"] "+padR(s.label,8);
        mvaddstr(r,x+2,lbl.substr(0,16).c_str());
        attroff(COLOR_PAIR(lc));
        attron(COLOR_PAIR(tc)|A_BOLD);
        std::ostringstream ts; ts<<std::fixed<<std::setprecision(0)<<s.temp<<"°C";
        mvaddstr(r,x+18,ts.str().c_str());
        attroff(COLOR_PAIR(tc)|A_BOLD);
        if(w>35){
            drawBarSimple(r,x+25,w-28,std::min(s.temp,100.0f),tc);
        }
        r++;
        sensor_count++;
    }
    if(sensor_count==0 && r<y+h-2){
        attron(COLOR_PAIR(C_DIM));
        mvaddstr(r,x+2,"  No sensors detected");
        attroff(COLOR_PAIR(C_DIM));
        r++;
    }
    if(st.batt.present && r<y+h-1){
        if(r<y+h-2){ divline(r,x,w); r++; }
        int bc=st.batt.charging?C_GREEN:(st.batt.pct<20?C_RED:C_YELLOW);
        attron(COLOR_PAIR(bc)|A_BOLD);
        std::string bs=" ⚡ "+std::to_string(st.batt.pct)+"% "+(st.batt.charging?"CHG":"DIS");
        mvaddstr(r,x+2,bs.c_str());
        attroff(COLOR_PAIR(bc)|A_BOLD);
    }
}

static void pMem(const AppState& st,int y,int x,int h,int w){
    if(h<6 || w<20) return;  // Increased min height from 5 to 6
    
    drawPanel(y,x,h,w,"MEMORY");
    int r=y+2;
    int bw=w-20;
    
    // RAM usage bar
    double rpct=st.mem.total>0 ? 100.0*st.mem.used()/st.mem.total : 0;
    attron(COLOR_PAIR(C_DIM)); mvaddstr(r,x+2,"Used:"); attroff(COLOR_PAIR(C_DIM));
    drawGradientBar(r,x+8,bw,rpct);
    r++;
    
    // Detailed memory stats (NEW - shows all values)
    attron(COLOR_PAIR(C_DIM));
    std::ostringstream stats;
    stats << " Total: " << st.mem.total/1024 << "MB  |  ";
    stats << "Used: " << st.mem.used()/1024 << "MB  |  ";
    stats << "Free: " << st.mem.free/1024 << "MB  |  ";
    stats << "Avail: " << st.mem.available/1024 << "MB";
    std::string stats_str = stats.str();
    if((int)stats_str.size() > w-4) stats_str = stats_str.substr(0, w-7) + "..";
    mvaddstr(r,x+2,stats_str.c_str());
    attroff(COLOR_PAIR(C_DIM));
    r++;
    
    // RAM Type Info (NEW)
    if(st.mem_type.detected && r<y+h-2){
        attron(COLOR_PAIR(C_CYAN)|A_BOLD);
        std::ostringstream type;
        type << " Type: " << st.mem_type.type;
        if(st.mem_type.speed != "Unknown") type << "  Speed: " << st.mem_type.speed;
        if(st.mem_type.form != "Unknown") type << "  Form: " << st.mem_type.form;
        mvaddstr(r,x+2,type.str().c_str());
        attroff(COLOR_PAIR(C_CYAN)|A_BOLD);
        r++;
    }
    
    // SWAP
    if(st.mem.swap_total>0 && r<y+h-2){
        double spct=st.mem.swap_total>0 ? 100.0*st.mem.swap_used()/st.mem.swap_total : 0;
        attron(COLOR_PAIR(C_DIM)); mvaddstr(r,x+2,"Swap:"); attroff(COLOR_PAIR(C_DIM));
        drawGradientBar(r,x+8,bw,spct);
        std::ostringstream swp;
        swp << " " << st.mem.swap_used()/1024 << "/" << st.mem.swap_total/1024 << "MB";
        attron(COLOR_PAIR(C_YELLOW));
        mvaddstr(r,x+8+bw+1,swp.str().c_str());
        attroff(COLOR_PAIR(C_YELLOW));
        r++;
    }
}

static void pNet(const AppState& st,int y,int x,int h,int w){
    if(h<5 || w<20) return;
    drawPanel(y,x,h,w,"NETWORK");
    int r=y+2;
    for(auto& iface:st.net_curr){
        if(r>=y+h-3) break;
        attron(COLOR_PAIR(C_TITLE)|A_BOLD);
        std::string nm=(iface.primary?"★ ":"  ")+iface.name;
        mvaddstr(r,x+2,padR(nm,12).c_str());
        attroff(COLOR_PAIR(C_TITLE)|A_BOLD);
        attron(COLOR_PAIR(C_GREEN)|A_BOLD); mvaddstr(r,x+15,"▼"); attroff(COLOR_PAIR(C_GREEN)|A_BOLD);
        attron(COLOR_PAIR(C_DEFAULT)); mvaddstr(r,x+17,padR(fmtRate(iface.rx_rate),10).c_str()); attroff(COLOR_PAIR(C_DEFAULT));
        attron(COLOR_PAIR(C_RED)|A_BOLD); mvaddstr(r,x+28,"▲"); attroff(COLOR_PAIR(C_RED)|A_BOLD);
        attron(COLOR_PAIR(C_DEFAULT)); mvaddstr(r,x+30,padR(fmtRate(iface.tx_rate),10).c_str()); attroff(COLOR_PAIR(C_DEFAULT));
        r++;
        auto rx_it = st.rx_hist.find(iface.name);
        if(rx_it != st.rx_hist.end()){
            attron(COLOR_PAIR(C_DIM)); mvaddstr(r,x+2,"▼"); attroff(COLOR_PAIR(C_DIM));
            drawSpark(r,x+4,(w-10)/2,rx_it->second,C_GREEN);
        }
        auto tx_it = st.tx_hist.find(iface.name);
        if(tx_it != st.tx_hist.end()){
            attron(COLOR_PAIR(C_DIM)); mvaddstr(r,x+4+(w-10)/2,"▲"); attroff(COLOR_PAIR(C_DIM));
            drawSpark(r,x+6+(w-10)/2,(w-10)/2,tx_it->second,C_RED);
        }
        r++;
    }
}

static void pDisk(const AppState& st,int y,int x,int h,int w){
    if(h<4 || w<20) return;
    drawPanel(y,x,h,w,"DISK");
    int r=y+2;
    int bw=w-20;
    for(auto& d:st.disks){
        if(r>=y+h-2) break;
        double dpct=d.total>0 ? 100.0*d.used/d.total : 0;
        std::string mnt=d.mount;
        if((int)mnt.size()>12) mnt=mnt.substr(0,11)+".";
        attron(COLOR_PAIR(C_DIM));
        mvaddstr(r,x+2,padR(" "+mnt,14).c_str());
        attroff(COLOR_PAIR(C_DIM));
        drawGradientBar(r,x+16,bw,dpct);
        // Size on same line as bar
        attron(COLOR_PAIR(C_CYAN));
        std::ostringstream sz;
        sz << " " << fmtBytes(d.used)<<"/"<<fmtBytes(d.total);
        mvaddstr(r,x+16+bw+1,sz.str().c_str());
        attroff(COLOR_PAIR(C_CYAN));
        r++;
        // I/O rates on NEXT line (FIXED - won't overflow)
        if(r<y+h-2){
            attron(COLOR_PAIR(C_CYAN)|A_DIM);
            std::ostringstream io;
            io << "   R:" << padR(fmtRate(d.read_rate),8) << "  W:" << fmtRate(d.write_rate);
            mvaddstr(r,x+2,io.str().c_str());
            attroff(COLOR_PAIR(C_CYAN)|A_DIM);
            r++;
        }
    }
}

static void pHW(const AppState& st,int y,int x,int h,int w){
    if(h<6 || w<20) return;
    drawPanel(y,x,h,w,"HARDWARE INFO");
    int r=y+2;
    auto row=[&](const std::string& lbl,const std::string& val){
        if(r>=y+h-1) return;
        attron(COLOR_PAIR(C_DIM));
        mvaddstr(r,x+2,padR(" "+lbl+":",14).c_str());
        attroff(COLOR_PAIR(C_DIM));
        attron(COLOR_PAIR(C_DEFAULT)|A_BOLD);
        std::string v=val;
        if((int)v.size()>w-18) v=v.substr(0,w-20)+"..";
        mvaddstr(r,x+16,v.c_str());
        attroff(COLOR_PAIR(C_DEFAULT)|A_BOLD);
        r++;
    };
    row("Model",st.hw.model);
    row("CPU",st.hw.cpu);
    row("GPU",st.hw.gpu);
    row("RAM",std::to_string(st.hw.ram_gb)+"GB");
    row("Cores",std::to_string(st.hw.cores)+"C / "+std::to_string(st.hw.threads)+"T");
    row("Network",st.hw.net_iface);
    row("MAC",st.hw.mac);
}

static void pProc(const AppState& st,int y,int x,int h,int w,int scroll,int selected){
    if(h<6 || w<30) return;
    drawPanel(y,x,h,w,"PROCESSES");
    int r=y+2;
    attron(COLOR_PAIR(C_TITLE)|A_BOLD|A_UNDERLINE);
    std::string hdr=padR("PID",6)+padR("NAME",18)+padR("CPU%",8)+padR("MEM",8)+"S";
    mvaddstr(r,x+2,hdr.substr(0,w-4).c_str());
    attroff(COLOR_PAIR(C_TITLE)|A_BOLD|A_UNDERLINE);
    r++;
    divline(r,x,w); r++;
    int bar_x=x+w-12, bar_w=10;
    int shown=0;
    for(int i=scroll;i<(int)st.procs.size() && r<y+h-2;i++,shown++){
        auto& p=st.procs[i];
        double mpct=st.mem.total>0 ? 100.0*p.mem_kb/st.mem.total : 0;
        int cc=usageColor(p.cpu_pct);
        bool hot=p.cpu_pct>50;
        bool is_selected = (i == selected);
        
        std::string name=p.name;
        if((int)name.size()>17) name=name.substr(0,16)+".";
        
        // Highlight selected row
        if(is_selected){
            attron(COLOR_PAIR(C_SELECTED));
            for(int cx=x; cx<x+w-1; cx++) mvaddch(r,cx,' ');
        }
        
        attron(COLOR_PAIR(cc)|(hot?A_BOLD:0));
        std::ostringstream ln;
        ln<<std::right<<std::setw(5)<<p.pid<<"  "
          <<std::left<<std::setw(17)<<name<<"  "
          <<std::right<<std::setw(6)<<std::fixed<<std::setprecision(0)<<p.cpu_pct<<"%"
          <<" "<<std::right<<std::setw(6)<<p.mem_kb/1024
          <<" "<<p.state;
        mvaddstr(r,x+2,ln.str().substr(0,w-4).c_str());
        attroff(COLOR_PAIR(cc)|(hot?A_BOLD:0));
        
        if(is_selected) attroff(COLOR_PAIR(C_SELECTED));
        
        if(bar_w>4) drawBarSimple(r,bar_x,bar_w,std::min(mpct*5.0,100.0),cc);
        r++;
    }
    if((int)st.procs.size()>shown && r<y+h-2){
        attron(COLOR_PAIR(C_DIM));
        mvaddstr(y+h-2,x+2,(" ↓ "+std::to_string((int)st.procs.size()-shown)+" more (↑↓)  K:Send Signal").c_str());
        attroff(COLOR_PAIR(C_DIM));
    }
}

//  LAYOUT SYSTEM
static int getLayoutIndex(){
    int idx = 0;
    if(g_cfg.cpu)  idx |= 1;
    if(g_cfg.mem)  idx |= 2;
    if(g_cfg.net)  idx |= 4;
    if(g_cfg.disk) idx |= 8;
    if(g_cfg.hw)   idx |= 16;
    if(g_cfg.proc) idx |= 32;
    return idx;
}

struct LayoutTemplate {
    int left_width_pct, right_width_pct;
    int cpu_h_pct, mem_h_pct, net_h_pct, disk_h_pct, hw_h_pct, proc_h_pct;
    bool proc_right, hw_right;
};

static LayoutTemplate getLayoutTemplate(int layout_idx, int avail_h){
    LayoutTemplate t = {60, 40, 40, 20, 15, 12, 25, 40, true, true};  // Disk reduced to 12%
    
    bool cpu  = (layout_idx & 1);
    bool mem  = (layout_idx & 2);
    bool net  = (layout_idx & 4);
    bool disk = (layout_idx & 8);
    bool hw   = (layout_idx & 16);
    bool proc = (layout_idx & 32);
    
    int left_active = cpu + mem + net + disk;
    if(left_active == 0) left_active = 1;
    
    if(proc){
        t.proc_right = true;
        t.proc_h_pct = 85;
        if(hw){
            t.hw_right = true;
            t.hw_h_pct = 25;
            t.proc_h_pct = 70;
            t.left_width_pct = 55;
            t.right_width_pct = 45;
        } else {
            t.hw_right = false;
            t.left_width_pct = 55;
            t.right_width_pct = 45;
        }
        if(cpu){
            if(left_active == 1) t.cpu_h_pct = 50;
            else if(left_active == 2) t.cpu_h_pct = 45;
            else if(left_active == 3) t.cpu_h_pct = 40;
            else t.cpu_h_pct = 35;
            int remaining = 100 - t.cpu_h_pct;
            int others = left_active - 1;
            if(others > 0){
                if(mem) t.mem_h_pct = remaining / others;
                if(net) t.net_h_pct = remaining / others;
                if(disk) t.disk_h_pct = remaining / others;
            }
        }
    }
    else if(hw){
        t.hw_right = true;
        t.proc_right = false;
        t.hw_h_pct = 35;
        t.left_width_pct = 60;
        t.right_width_pct = 40;
        if(cpu){
            if(left_active == 1) t.cpu_h_pct = 55;
            else if(left_active == 2) t.cpu_h_pct = 45;
            else if(left_active == 3) t.cpu_h_pct = 40;
            else t.cpu_h_pct = 35;
            int remaining = 100 - t.cpu_h_pct;
            int others = left_active - 1;
            if(others > 0){
                if(mem && net && disk) { t.mem_h_pct = 22; t.net_h_pct = 22; t.disk_h_pct = 21; }
                else if(mem && net) { t.mem_h_pct = remaining/2; t.net_h_pct = remaining/2; }
                else if(mem && disk) { t.mem_h_pct = remaining/2; t.disk_h_pct = remaining/2; }
                else if(net && disk) { t.net_h_pct = remaining/2; t.disk_h_pct = remaining/2; }
                else if(mem) t.mem_h_pct = remaining;
                else if(net) t.net_h_pct = remaining;
                else if(disk) t.disk_h_pct = remaining;
            }
        }
    }
    else {
        t.hw_right = false;
        t.proc_right = false;
        t.left_width_pct = 100;
        t.right_width_pct = 0;
        if(cpu){
            if(left_active == 1) t.cpu_h_pct = 60;
            else if(left_active == 2) t.cpu_h_pct = 50;
            else if(left_active == 3) t.cpu_h_pct = 45;
            else t.cpu_h_pct = 40;
            int remaining = 100 - t.cpu_h_pct;
            int others = left_active - 1;
            if(others > 0){
                if(mem && net && disk) { t.mem_h_pct = 20; t.net_h_pct = 20; t.disk_h_pct = 20; }
                else if(mem && net) { t.mem_h_pct = remaining/2; t.net_h_pct = remaining/2; }
                else if(mem && disk) { t.mem_h_pct = remaining/2; t.disk_h_pct = remaining/2; }
                else if(net && disk) { t.net_h_pct = remaining/2; t.disk_h_pct = remaining/2; }
                else if(mem) t.mem_h_pct = remaining;
                else if(net) t.net_h_pct = remaining;
                else if(disk) t.disk_h_pct = remaining;
            }
        }
    }
    
    if(t.cpu_h_pct < 20) t.cpu_h_pct = 20;
    if(t.mem_h_pct < 12) t.mem_h_pct = 12;  // Increased for more memory info
    if(t.net_h_pct < 10) t.net_h_pct = 10;
    if(t.disk_h_pct < 10) t.disk_h_pct = 10;  // Reduced from 15
    if(t.hw_h_pct < 15) t.hw_h_pct = 15;
    if(t.proc_h_pct < 15) t.proc_h_pct = 15;
    
    return t;
}

static void applyLayout(const AppState& st,int rows,int cols){
    int y0=3, bot=rows-1;
    int avail_h=bot-y0;
    if(avail_h<15 || cols<70) return;
    
    int layout_idx = getLayoutIndex();
    LayoutTemplate t = getLayoutTemplate(layout_idx, avail_h);
    
    int half = (cols * t.left_width_pct) / 100;
    int right_x = half + 1;
    int right_w = cols - right_x - 1;
    int y=y0;
    
    if(g_cfg.cpu && t.left_width_pct > 0){
        int h = (avail_h * t.cpu_h_pct) / 100;
        if(h < 8) h = 8;
        pCPU(st, y, 0, h, half);
        y += h;
    }
    if(g_cfg.mem && t.left_width_pct > 0 && y < bot-5){
        int h = (avail_h * t.mem_h_pct) / 100;
        if(h < 6) h = 6;  // Increased minimum for memory info
        pMem(st, y, 0, h, half);
        y += h;
    }
    if(g_cfg.disk && t.left_width_pct > 0 && y < bot-5){
        int h = (avail_h * t.disk_h_pct) / 100;
        if(h < 4) h = 4;  // Reduced minimum
        pDisk(st, y, 0, h, half);
        y += h;
    }
    if(g_cfg.net && t.left_width_pct > 0 && y < bot-5){
        int h = (avail_h * t.net_h_pct) / 100;
        if(h < 5) h = 5;
        pNet(st, y, 0, h, half);
        y += h;
    }
    
    if(g_cfg.hw && t.hw_right && t.right_width_pct > 0){
        int h = (avail_h * t.hw_h_pct) / 100;
        if(h < 8) h = 8;
        if(!st.hw.loaded) const_cast<AppState&>(st).hw=readHW(st.meta);
        pHW(st, y0, right_x, h, right_w);
    }
    
    if(g_cfg.proc && t.right_width_pct > 0){
        int proc_y = (g_cfg.hw && t.hw_right) ? (y0 + (avail_h * t.hw_h_pct) / 100 + 1) : y0;
        int proc_h = bot - proc_y - 1;
        if(proc_h < 6) proc_h = 6;
        pProc(st, proc_y, right_x, proc_h, right_w, g_cfg.proc_scroll, g_cfg.proc_selected);
    }
}

//  MAIN
int main(){
    setlocale(LC_ALL,"");
    signal(SIGWINCH,[](int){g_resize=true;});
    signal(SIGINT,[](int){g_quit=true;});
    signal(SIGTERM,[](int){g_quit=true;});
    
    initscr(); cbreak(); noecho();
    keypad(stdscr,TRUE); nodelay(stdscr,TRUE); curs_set(0);
    timeout(50);
    
    if(!has_colors()){endwin();fprintf(stderr,"No color support.\n");return 1;}
    start_color(); use_default_colors();
    applyTheme();
    bkgd(COLOR_PAIR(C_DEFAULT));
    
    AppState st;
    st.meta=readMeta();
    st.cpu_curr=readCPU(); st.net_curr=readNet();
    st.disk_io_curr=readDiskIO(); st.sensors=readSensors();
    st.mem_type = readMemType();  // Read RAM type on startup
    usleep(150000);
    updateState(st);
    
    int loop_ms=50, tick_max=g_cfg.update_ms/loop_ms, tick=tick_max;
    
    while(!g_quit){
        if(g_resize){
            endwin(); refresh(); applyTheme();
            bkgd(COLOR_PAIR(C_DEFAULT)); g_resize=false;
            continue;
        }
        if(tick>=tick_max){
            updateState(st);
            tick=0;
        }
        tick++;
        
        int rows,cols; getmaxyx(stdscr,rows,cols);
        erase();
        
        drawTopBar(st,cols);
        drawBottomBar(rows,cols);
        
        // Signal dialog overlay
       // Signal dialog overlay
        if(g_cfg.signal_mode){
        handleSignalUI(st);  // ← New function
        continue;
        }
        
        if(rows>=22 && cols>=80){
            applyLayout(st,rows,cols);
        }else{
            attron(COLOR_PAIR(C_RED)|A_BOLD);
            std::ostringstream sz;
            sz<<"Terminal too small! Need 80x22, have "<<cols<<"x"<<rows;
            mvaddstr(rows/2,std::max(0,(cols-(int)sz.str().size())/2),sz.str().c_str());
            attroff(COLOR_PAIR(C_RED)|A_BOLD);
        }
        
        refresh();
        g_fps.tick();
        
        int ch=getch();
        int vis=rows-10;
        
        if(ch!=ERR){
            switch(ch){
                case '1': g_cfg.cpu=!g_cfg.cpu; break;
                case '2': g_cfg.mem=!g_cfg.mem; break;
                case '3': g_cfg.net=!g_cfg.net; break;
                case '4': g_cfg.disk=!g_cfg.disk; break;
                case '5':
                    g_cfg.hw=!g_cfg.hw;
                    if(g_cfg.hw && !st.hw.loaded) st.hw=readHW(st.meta);
                    break;
                case '6': g_cfg.proc=!g_cfg.proc; break;
                case 'g': case 'G': g_cfg.graphs=!g_cfg.graphs; break;
                case 'f': case 'F': g_cfg.proc_fullscreen=!g_cfg.proc_fullscreen; break;
                case 't': case 'T': g_theme=(Theme)((g_theme+1)%3); applyTheme(); break;
                case 'q': case 'Q': g_quit=true; break;
                case 'k': case 'K': case '\n': case KEY_ENTER:
                    if(g_cfg.proc && !st.procs.empty()){
                        g_cfg.signal_mode = true;
                        g_signal_selected = 0;
                    }
                    break;
                case '+': case '=':
                    g_cfg.update_ms=std::max(100,g_cfg.update_ms-100);
                    tick_max=g_cfg.update_ms/loop_ms;
                    break;
                case '-':
                    g_cfg.update_ms=std::min(5000,g_cfg.update_ms+100);
                    tick_max=g_cfg.update_ms/loop_ms;
                    break;
                case KEY_UP:
                    if(g_cfg.proc_scroll>0) g_cfg.proc_scroll--;
                    if(g_cfg.proc_selected>0) g_cfg.proc_selected--;
                    break;
                case KEY_DOWN:
                    if(g_cfg.proc_scroll<(int)st.procs.size()-vis) g_cfg.proc_scroll++;
                    if(g_cfg.proc_selected<(int)st.procs.size()-1) g_cfg.proc_selected++;
                    break;
                case KEY_PPAGE: g_cfg.proc_scroll=std::max(0,g_cfg.proc_scroll-vis); break;
                case KEY_NPAGE: g_cfg.proc_scroll=std::min((int)st.procs.size()-1,g_cfg.proc_scroll+vis); break;
                case KEY_HOME: g_cfg.proc_scroll=0; g_cfg.proc_selected=0; break;
                case KEY_RESIZE: g_resize=true; break;
            }
        }
        
        usleep(loop_ms*1000);
    }
    
    endwin();
    printf("\n  ◈ ttop — goodbye\n\n");
    return 0;
}
