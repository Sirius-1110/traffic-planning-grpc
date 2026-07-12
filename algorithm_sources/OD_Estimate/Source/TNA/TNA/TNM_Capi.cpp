#include "header\stdafx.h"
#include "header\TNM_Algorithm.h"
#include "header\TNM_utility.h"

#include <string>

using namespace std;

// 辅助函数：将通用句柄转换为 OD_ESTIMATION 指针
static inline OD_ESTIMATION* FromHandle(void* handle)
{
    return reinterpret_cast<OD_ESTIMATION*>(handle);
}

// 默认算法参数配置：保持与 TNADriver.cpp 的 JSON 驱动逻辑一致
static void ApplyDefaultParameters(OD_ESTIMATION* odes)
{
    if (odes == NULL)
    {
        return;
    }

    TNM_FloatFormat::SetFormat(18, 6);

    odes->SetRoleName(TNM_DEFAULT_ROLE);
    odes->SetNetworkTableName(string());
    odes->SetODTableName(string());
    odes->SetObservedTableName(string());

    odes->SetGamma(0.5);
    odes->SetOblink_ratio(0.3);
    odes->SetMainloop_maxiter(300);
    odes->SetMainloop_conv(1e-4);
    odes->SetArmijo_maxiter(5);
    odes->SetArmijo_stopcriterion(100);
    odes->SetArmijo_coefficient(10.0);
    odes->SetArmijo_alphamax(1000.0);
    odes->SetConv(1e-6);
    odes->SetMaxIter(500);
    odes->SetLPF(BPRLK);
    odes->SetCostScalar(60);
    odes->SetCostCoef(1.0, 0.0);
    odes->SetUseExternalObservedFlow(false);

    odes->reportIterHistory      = false;
    odes->reportLinkDetail       = false;
    odes->reportPathDetail       = false;
    odes->reportDemandDetail     = true;
    odes->reportlinkinforDetail  = true;
    odes->reportUpperObjective   = true;

    odes->SetResultTablePrefix("");

}

// 创建估计器实例并初始化默认参数，成功返回句柄
extern "C" __declspec(dllexport) void* TNM_CreateEstimator()
{
    try
    {
        OD_ESTIMATION* instance = new OD_ESTIMATION;
        ApplyDefaultParameters(instance);
        return reinterpret_cast<void*>(instance);
    }
    catch (...)
    {
        return NULL;
    }
}

// 销毁估计器实例，释放关联资源
extern "C" __declspec(dllexport) int TNM_DestroyEstimator(void* handle)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    delete odes;
    return 0;
}

// 重置估计器参数为默认配置
extern "C" __declspec(dllexport) int TNM_ResetDefaults(void* handle)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    ApplyDefaultParameters(odes);
    return 0;
}

// 设置数据库连接串，空指针视为清空
extern "C" __declspec(dllexport) int TNM_SetDbConnStr(void* handle, const char* conn)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetDbConnStr(conn == NULL ? "" : conn);
    return 0;
}

// 设置结果写库时使用的表名前缀
extern "C" __declspec(dllexport) int TNM_SetResultTablePrefix(void* handle, const char* prefix)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetResultTablePrefix(prefix == NULL ? "" : prefix);
    return 0;
}

// 设置角色名称，用于拼接表名
extern "C" __declspec(dllexport) int TNM_SetRoleName(void* handle, const char* role)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    string value = (role == NULL) ? string() : string(role);
    odes->SetRoleName(value);
    return 0;
}

// 设置网络结构表名称
extern "C" __declspec(dllexport) int TNM_SetNetworkTableName(void* handle, const char* name)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    string value = (name == NULL) ? string() : string(name);
    odes->SetNetworkTableName(value);
    return 0;
}

// 设置 OD 表名称
extern "C" __declspec(dllexport) int TNM_SetODTableName(void* handle, const char* name)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    string value = (name == NULL) ? string() : string(name);
    odes->SetODTableName(value);
    return 0;
}

// 设置观测流量表名称
extern "C" __declspec(dllexport) int TNM_SetObservedTableName(void* handle, const char* name)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    string value = (name == NULL) ? string() : string(name);
    odes->SetObservedTableName(value);
    return 0;
}

// 获取当前网络表名称，失败返回 NULL
extern "C" __declspec(dllexport) const char* TNM_GetNetworkTableName(void* handle)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return NULL;
    }
    return odes->GetNetworkTableName().c_str();
}

// 获取当前 OD 表名称，失败返回 NULL
extern "C" __declspec(dllexport) const char* TNM_GetODTableName(void* handle)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return NULL;
    }
    return odes->GetODTableName().c_str();
}

// 获取当前观测表名称，失败返回 NULL
extern "C" __declspec(dllexport) const char* TNM_GetObservedTableName(void* handle)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return NULL;
    }
    return odes->GetObservedTableName().c_str();
}

// 获取当前角色名称，失败返回 NULL
extern "C" __declspec(dllexport) const char* TNM_GetRoleName(void* handle)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return NULL;
    }
    return odes->GetRoleName().c_str();
}

// 设置 gamma1 与 gamma2 算法参数
extern "C" __declspec(dllexport) int TNM_SetGamma(void* handle, double gamma1Value, double gamma2Value)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetGamma(static_cast<floatType>(gamma1Value), static_cast<floatType>(gamma2Value));
    return 0;
}

// 设置观测链路使用比例
extern "C" __declspec(dllexport) int TNM_SetObservedRatio(void* handle, double ratio)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetOblink_ratio(static_cast<floatType>(ratio));
    return 0;
}

// 设置主循环最大迭代次数
extern "C" __declspec(dllexport) int TNM_SetMainLoopMaxIter(void* handle, int iter)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetMainloop_maxiter(iter);
    return 0;
}

// 设置主循环收敛阈值
extern "C" __declspec(dllexport) int TNM_SetMainLoopConv(void* handle, double conv)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetMainloop_conv(static_cast<floatType>(conv));
    return 0;
}

// 设置 Armijo 最大迭代次数
extern "C" __declspec(dllexport) int TNM_SetArmijoMaxIter(void* handle, int iter)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetArmijo_maxiter(iter);
    return 0;
}

// 设置 Armijo 停止准则阈值
extern "C" __declspec(dllexport) int TNM_SetArmijoStopCriterion(void* handle, double stop)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetArmijo_stopcriterion(static_cast<floatType>(stop));
    return 0;
}

// 设置 Armijo 系数
extern "C" __declspec(dllexport) int TNM_SetArmijoCoefficient(void* handle, double coef)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetArmijo_coefficient(static_cast<floatType>(coef));
    return 0;
}

// 设置 Armijo 步长上限
extern "C" __declspec(dllexport) int TNM_SetArmijoAlphaMax(void* handle, double alpha)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetArmijo_alphamax(static_cast<floatType>(alpha));
    return 0;
}

// 设置全局收敛阈值
extern "C" __declspec(dllexport) int TNM_SetConvCriterion(void* handle, double conv)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetConv(static_cast<floatType>(conv));
    return 0;
}

// 设置全局最大迭代次数
extern "C" __declspec(dllexport) int TNM_SetMaxIter(void* handle, int iter)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetMaxIter(iter);
    return 0;
}

// 设置链路性能函数类型
extern "C" __declspec(dllexport) int TNM_SetLinkPerformance(void* handle, int linkType)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetLPF(static_cast<TNM_LINKTYPE>(linkType));
    return 0;
}

// 设置成本缩放系数
extern "C" __declspec(dllexport) int TNM_SetCostScalar(void* handle, double scalar)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetCostScalar(static_cast<floatType>(scalar));
    return 0;
}

// 设置成本函数中的时间与距离系数
extern "C" __declspec(dllexport) int TNM_SetCostCoefficient(void* handle, double timeCoef, double distCoef)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetCostCoef(timeCoef, distCoef);
    return 0;
}

// 控制是否输出迭代历史
extern "C" __declspec(dllexport) int TNM_EnableReportIter(void* handle, int enabled)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->reportIterHistory = (enabled != 0);
    return 0;
}

// 控制是否输出链路细节
extern "C" __declspec(dllexport) int TNM_EnableReportLinkDetail(void* handle, int enabled)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->reportLinkDetail = (enabled != 0);
    return 0;
}

// 控制是否输出路径细节
extern "C" __declspec(dllexport) int TNM_EnableReportPathDetail(void* handle, int enabled)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->reportPathDetail = (enabled != 0);
    return 0;
}

// 控制是否输出需求细节
extern "C" __declspec(dllexport) int TNM_EnableReportDemand(void* handle, int enabled)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->reportDemandDetail = (enabled != 0);
    return 0;
}

// 控制是否输出链路信息细节
extern "C" __declspec(dllexport) int TNM_EnableReportLinkInfo(void* handle, int enabled)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->reportlinkinforDetail = (enabled != 0);
    return 0;
}

// 控制是否输出上层目标值
extern "C" __declspec(dllexport) int TNM_EnableReportUpperObjective(void* handle, int enabled)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->reportUpperObjective = (enabled != 0);
    return 0;
}

// 控制是否使用外部观测流量
extern "C" __declspec(dllexport) int TNM_SetUseExternalObserved(void* handle, int enabled)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->SetUseExternalObservedFlow(enabled != 0);
    return 0;
}

// 根据输入输出路径构建网络数据
extern "C" __declspec(dllexport) int TNM_Build(void* handle, const char* inFile, const char* outFile, int format)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    const string inPath = (inFile == NULL ? "" : inFile);
    const string outPath = (outFile == NULL ? "" : outFile);
    return odes->Build(inPath, outPath, static_cast<MATINFORMAT>(format));
}

// 执行求解流程并返回状态码
extern "C" __declspec(dllexport) int TNM_Solve(void* handle)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    return static_cast<int>(odes->Solve());
}

// 执行完整流程封装入口
extern "C" __declspec(dllexport) int TNM_OverallProcess(void* handle)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    odes->overall_process();
    return 0;
}

// 输出本地报表文件
extern "C" __declspec(dllexport) int TNM_Report(void* handle)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    return odes->Report();
}

// 将结果写入 PostgreSQL 报表
extern "C" __declspec(dllexport) int TNM_ReportPG(void* handle)
{
    OD_ESTIMATION* odes = FromHandle(handle);
    if (odes == NULL)
    {
        return -1;
    }
    return odes->ReportPG();
}
