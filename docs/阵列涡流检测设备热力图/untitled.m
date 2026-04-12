clc; clear; close all;

%% ========== 1) 选择 CSV ==========
[file, path] = uigetfile('*.csv', '选择 CSV 文件');
if isequal(file, 0)
    error('未选择 CSV 文件。');
end
csvFile = fullfile(path, file);

%% ========== 2) 读取表 ==========
T = readtable(csvFile, 'VariableNamingRule', 'preserve');

if ~ismember('FrameSequence', T.Properties.VariableNames)
    error('CSV 缺少 FrameSequence');
end
if ~ismember('TimestampUnixMs', T.Properties.VariableNames)
    error('CSV 缺少 TimestampUnixMs');
end
if ~ismember('CellCount', T.Properties.VariableNames)
    error('CSV 缺少 CellCount');
end

frameSeq = T.("FrameSequence");
timestampMs = T.("TimestampUnixMs");
cellCount = T.("CellCount");

nFrames = height(T);
nChannels = double(cellCount(1));

fprintf('读取文件: %s\n', csvFile);
fprintf('帧数: %d\n', nFrames);
fprintf('通道/位置数: %d\n', nChannels);

%% ========== 3) 判断前缀 ==========
prefix = detectPrefix(T);
if isempty(prefix)
    error('未检测到 PosXX_Amp 或 ChXX_Amp 列。');
end
fprintf('检测到前缀: %s\n', prefix);

if strcmp(prefix, 'Pos')
    yLabelText = '显示位置';
else
    yLabelText = '通道';
end

%% ========== 4) 读数据 ==========
Amp   = getFieldMatrix(T, prefix, nChannels, 'Amp');
Phase = getFieldMatrix(T, prefix, nChannels, 'Phase');
X     = getFieldMatrix(T, prefix, nChannels, 'X');
Y     = getFieldMatrix(T, prefix, nChannels, 'Y');

Amp   = fillmissing(Amp, 'constant', 0);
Phase = fillmissing(Phase, 'constant', 0);
X     = fillmissing(X, 'constant', 0);
Y     = fillmissing(Y, 'constant', 0);

%% ========== 5) 横轴 ==========
useTimeAxis = true;
if useTimeAxis
    xAxis = (timestampMs - timestampMs(1)) / 1000.0;
    xLabelText = '时间 (s)';
else
    xAxis = frameSeq;
    xLabelText = '帧号';
end

yAxis = 0:nChannels-1;

%% ========== 6) 生成“幅值+相位”颜色图 ==========
RGB_AmpPhase = makeOriginalColorImage(Amp, Phase);

%% ========== 7) 生成“实部+虚部”颜色图 ==========
MagXY = hypot(X, Y);
AngXY = atan2d(Y, X);
RGB_XY = makeOriginalColorImage(MagXY, AngXY);

%% ========== 8) 画图 ==========
figure('Name', '按DataConvertor 规则成像', ...
       'Color', 'w', 'Position', [80, 60, 1300, 850]);

subplot(2,1,1);
image(xAxis, yAxis, RGB_AmpPhase);
set(gca, 'YDir', 'normal');
xlabel(xLabelText);
ylabel(yLabelText);
title('幅值 + 相位');
grid on;

subplot(2,1,2);
image(xAxis, yAxis, RGB_XY);
set(gca, 'YDir', 'normal');
xlabel(xLabelText);
ylabel(yLabelText);
title('实部 + 虚部');
grid on;

sgtitle('');


%% ================= 局部函数 =================

function prefix = detectPrefix(T)
    names = T.Properties.VariableNames;

    hasPos = any(startsWith(names, 'Pos00_Amp')) || any(contains(names, '_Amp') & startsWith(names, 'Pos'));
    hasCh  = any(startsWith(names, 'Ch00_Amp'))  || any(contains(names, '_Amp') & startsWith(names, 'Ch'));

    if hasPos
        prefix = 'Pos';
    elseif hasCh
        prefix = 'Ch';
    else
        prefix = '';
    end
end

function M = getFieldMatrix(T, prefix, nChannels, fieldName)
    nFrames = height(T);
    M = nan(nFrames, nChannels);

    for ch = 0:nChannels-1
        colName = sprintf('%s%02d_%s', prefix, ch, fieldName);

        if ~ismember(colName, T.Properties.VariableNames)
            error('CSV 中缺少列: %s', colName);
        end

        M(:, ch+1) = T.(colName);
    end
end

function RGB = makeOriginalColorImage(Amp, Phase)
    % 严格按原程序逻辑：
    % min = log10(0.05)
    % max = log10(0.3)
    % logAmp = log10(amp)
    % 裁剪后归一化到 [0,1]
    % HSVToRGB(phase, 1, logAmp)

    AmpSafe = Amp;
    AmpSafe(AmpSafe <= 0) = 1e-12;   % 防止 log10(0)

    minVal = log10(0.05);
    maxVal = log10(0.3);

    logAmp = log10(AmpSafe);
    logAmp(logAmp < minVal) = minVal;
    logAmp(logAmp > maxVal) = maxVal;

    value = (logAmp - minVal) / (maxVal - minVal);
    value = max(min(value, 1), 0);

    hue = mod(Phase, 360) / 360.0;   % 归一化到 [0,1)
    sat = ones(size(hue));

    % 转成图像格式：[通道, 帧, 3]
    H = hue';
    S = sat';
    V = value';

    HSV = cat(3, H, S, V);
    RGB = hsv2rgb(HSV);
end