#pragma once
#include "Engine/Component.h"

class PositionInterpolatorComponent : public Component
{
    DECLARE_COMPONENT_TYPEINFO(PositionInterpolatorComponent, Component);
    DECLARE_COMPONENT_GENERIC_FACTORY(PositionInterpolatorComponent);

public:
    PositionInterpolatorComponent(const ComponentTypeInfo *pTypeInfo = &s_typeInfo);
    virtual ~PositionInterpolatorComponent();

    // Property accessors
    const float GetMoveDuration() const { return m_moveDuration; }
    const float GetPauseDuration() const { return m_pauseDuration; }
    const bool GetReverseCycle() const { return m_reverseCycle; }
    const uint32 GetRepeatCount() const { return m_repeatCount; }
    const EasingFunction::Type GetEasingFunction() const { return m_easingFunction; }
    const bool GetAutoActivate() const { return m_autoActivate; }

    // Property mutators
    void SetMoveDuration(float moveDuration);
    void SetPauseDuration(float pauseDuration);
    void SetReverseCycle(bool autoReverse);
    void SetRepeatCount(uint32 repeatCount);
    void SetEasingFunction(EasingFunction::Type easingFunction);
    void SetAutoActivate(bool autoActivate);

    // Creator
    void Create(const float3 &moveVector = float3::UnitZ, float moveDuration = 1.0f, float pauseDuration = 0.0f, bool reverseCycle = true, uint32 repeatCount = 0, EasingFunction::Type easingFunction = EasingFunction::Linear, bool autoActivate = true);

    // Reset to the initial state
    void Reset();

    // Activate/resume the mover
    void Activate();

    // Deactivate/pause the mover
    void Deactivate();

    // Events
    virtual bool Initialize() override;
    virtual void OnAddToEntity(Entity *pEntity) override;
    virtual void OnRemoveFromEntity(Entity *pEntity) override;
    virtual void OnAddToWorld(World *pWorld) override;
    virtual void OnRemoveFromWorld(World *pWorld) override;
    virtual void OnLocalTransformChange() override;
    virtual void OnEntityTransformChange() override;
    virtual void Update(float timeSinceLastUpdate) override;

private:
    float m_moveDuration;
    float m_pauseDuration;
    bool m_reverseCycle;
    uint32 m_repeatCount;
    EasingFunction::Type m_easingFunction;
    bool m_autoActivate;

    bool m_active;
    Interpolator<float3> m_interpolator;
};

