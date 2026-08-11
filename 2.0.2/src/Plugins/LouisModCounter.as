const string INSTANCE_COUNT_VAR = "louismod_instance_count";
const string COUNTER_VISIBLE_VAR = "louismod_counter_visible";
const int MIN_INSTANCE_COUNT = 1;
const int MAX_INSTANCE_COUNT = 32;

int ClampInstanceCount(int count)
{
    if (count < MIN_INSTANCE_COUNT) {
        return MIN_INSTANCE_COUNT;
    }
    if (count > MAX_INSTANCE_COUNT) {
        return MAX_INSTANCE_COUNT;
    }
    return count;
}

int GetInstanceCount()
{
    int count = ClampInstanceCount(
        int(GetVariableDouble(INSTANCE_COUNT_VAR)));
    if (count != int(GetVariableDouble(INSTANCE_COUNT_VAR))) {
        SetVariable(INSTANCE_COUNT_VAR, count);
    }
    return count;
}

void SetInstanceCount(int count)
{
    SetVariable(INSTANCE_COUNT_VAR, ClampInstanceCount(count));
}

void Main()
{
    RegisterVariable(INSTANCE_COUNT_VAR, 7.0);
    RegisterVariable(COUNTER_VISIBLE_VAR, false);

    SetInstanceCount(GetInstanceCount());
    SetVariable(COUNTER_VISIBLE_VAR, false);
}

void Render()
{
    if (!GetVariableBool(COUNTER_VISIBLE_VAR)) {
        return;
    }

    int flags = UI::WindowFlags::NoResize |
                UI::WindowFlags::NoCollapse |
                UI::WindowFlags::AlwaysAutoResize |
                UI::WindowFlags::NoDocking;

    if (UI::Begin("Multi-Bruteforce##LouisModReplayMenuOverlay", flags)) {
        int count = GetInstanceCount();

        UI::Text("Multi-BF Instances");
        UI::SameLine();
        if (UI::Button("-##LouisModInstanceMinus")) {
            SetInstanceCount(count - 1);
            count = GetInstanceCount();
        }
        UI::SameLine();
        UI::Text("" + count);
        UI::SameLine();
        if (UI::Button("+##LouisModInstancePlus")) {
            SetInstanceCount(count + 1);
        }
    }
    UI::End();
}

void OnDisabled()
{
    SetVariable(COUNTER_VISIBLE_VAR, false);
}

PluginInfo@ GetPluginInfo()
{
    auto info = PluginInfo();
    info.Name = "LouisMod Instance Counter";
    info.Author = "LouisMod";
    info.Version = "2.0.0";
    info.Description =
        "Selects how many Multi-Bruteforce instances LouisMod starts.";
    return info;
}
