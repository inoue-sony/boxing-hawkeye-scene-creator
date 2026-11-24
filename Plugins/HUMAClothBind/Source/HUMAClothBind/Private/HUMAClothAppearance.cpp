#include "HUMAClothAppearance.h"
#include "HUMAClothBindComponent.h"
#include "Materials/MaterialInterface.h"

bool UHUMAClothAppearanceLibrary::ApplyTeamAppearance(AActor* Player, int32 TeamId, UHUMAClothAppearance* Appearance)
{
    const FHUMAClothTeamAppearance* Team = Appearance ? Appearance->Teams.Find(TeamId) : nullptr;
    if (!IsValid(Player) || !Team || Team->MaterialSlots.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("HUMAClothBind: No appearance configuration for team %d on %s."), TeamId, *GetNameSafe(Player));
        return false;
    }
    for (const auto& Item : Team->MaterialSlots)
    {
        if (!IsValid(Item.Value.Get()))
        {
            UE_LOG(LogTemp, Error, TEXT("HUMAClothBind: Team %d material override %s is null/invalid; no garments were changed."), TeamId, *Item.Key.ToString());
            return false;
        }
    }
    TArray<AActor*> Actors = {Player};
    Player->GetAttachedActors(Actors, false, true);
    int32 ChangedSections = 0;
    for (AActor* Actor : Actors)
    {
        if (!IsValid(Actor) || (Actor != Player && !Actor->IsOwnedBy(Player))) continue;
        TInlineComponentArray<UHUMAClothBindComponent*> Bindings(Actor);
        for (UHUMAClothBindComponent* Binding : Bindings)
        {
            const int32 Changed = Binding->ApplyMaterialSlots(Team->MaterialSlots);
            if (Changed == INDEX_NONE) return false;
            ChangedSections += Changed;
        }
    }
    if (ChangedSections == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("HUMAClothBind: Team %d material slots matched no initialized garment sections on %s."), TeamId, *Player->GetName());
        return false;
    }
    return true;
}
