#pragma once

#include <functional>
#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // Up to four value channels over time on one graph (a sequencer track's X / Y / Z, a colour's R / G / B / A): each channel's
    // curve as sampled dots, its keys as round dots, the playhead, the zero line, the value range in the corner. Time runs from 0 to
    // Duration across the width; the value range follows the curves with a margin (held still while a key is dragged).
    //
    // Click a key's dot to select it (OnSelectKey); drag it up or down to change that channel's value; with Shift also its time
    // (SnapTime, kept within 0..Duration). OnChanging after each step, OnChanged when the drag ends (sort the keys then).
    //
    // The data stays the caller's: read through the functions below every frame. Colours: theme tokens CurveBackground, CurveX /
    // CurveY / CurveZ / CurveW (a single channel: CurveSingle), CurveKeySelected, CurvePlayhead, CurveZero.
    class UIChannelCurves : public UIElement
    {
    public:
        UIChannelCurves();

        std::function<int()> ChannelCount;                  // 0..4; 0: no values (NoValuesText); -1: nothing selected (NothingText)
        std::function<int()> KeyCount;
        std::function<float(int key)> KeyTime;
        std::function<float(int key, int channel)> KeyValue;
        std::function<float(float time, int channel)> Evaluate;
        std::function<void(int key, int channel, float time, float value)> SetKey;
        std::function<float()> Duration;                    // seconds (at least 0.01 is used)
        std::function<float()> Playhead;                    // empty: none drawn
        std::function<int()> SelectedKey;                   // -1: none
        std::function<float(float time)> SnapTime;          // empty: free

        std::function<void(int key)> OnSelectKey;
        std::function<void()> OnChanging;
        std::function<void()> OnChanged;

        std::string NothingText = "Nothing to show.";      // ChannelCount -1 or unset, KeyCount unset
        std::string NoValuesText = "No values.";
        std::string NoKeysText = "No keys yet.";

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

        float GetLow() const { return m_Low; }
        float GetHigh() const { return m_High; }
        UIVec2 ToScreen(float time, float value) const;

    protected:
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        void UpdateRange();
        int Channels() const;

        int m_Key = -1, m_Channel = 0;
        float m_Low = 0.0f, m_High = 1.0f, m_Duration = 1.0f;
    };
}
