/*
 * GestionEcrans.cpp
 * -----------------
 * DLL native pour basculer entre un mode "TV" (écran unique)
 * et un mode "Bureau" (deux moniteurs LG côte à côte).
 *
 * API MODERNE CCD (Connecting and Configuring Displays) :
 *   Abandonne l'ancienne API ChangeDisplaySettingsExW au profit de :
 *     1. GetDisplayConfigBufferSizes + QueryDisplayConfig(QDC_ALL_PATHS)
 *        -> lire TOUTE la topologie connue du systeme.
 *     2. DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME)
 *        -> identifier chaque cible (nom EDID, device path).
 *     3. Modifier les flags DISPLAYCONFIG_PATH_ACTIVE sur chaque chemin.
 *     4. SetDisplayConfig(SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
 *        SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE)
 *        -> appliquer la nouvelle topologie d'un seul coup.
 *
 *   Cible : Windows 10 et superieur.
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <vector>
#include <algorithm>
#include "GestionEcrans.h"

/* ============================================================================
 * SECTION CONFIGURATION
 * ============================================================================
 *
 * Le systeme lit le nom convivial (monitorFriendlyDeviceName) et le
 * chemin materiel (monitorDevicePath) de chaque cible CCD.
 * Le device path contient le PnP ID (ex: "HEC81C9", "GSM59A8").
 *
 * ---- MODIFIE CES LISTES SELON TON MATERIEL ----
 */
static const wchar_t* KEYWORDS_BUREAU[] = { L"LG", L"GSM", L"23EA53", NULL };
static const wchar_t* KEYWORDS_TV[]     = { L"HISENSE", L"HEC", NULL };

enum class Role { INCONNU, TV, BUREAU };

/* ============================================================================
 * LOGGING (operations.log a cote de la DLL)
 * ============================================================================ */

static FILE* g_logFile = NULL;

static void LogOuvrir()
{
    if (g_logFile) return;
    wchar_t dllPath[MAX_PATH] = {};
    HMODULE hSelf = NULL;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&LogOuvrir, &hSelf);
    GetModuleFileNameW(hSelf, dllPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(dllPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    wcscat_s(dllPath, MAX_PATH, L"operations.log");
    _wfopen_s(&g_logFile, dllPath, L"w, ccs=UTF-8");
}

static void LogFermer()
{
    if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
}

static void Log(const wchar_t* fmt, ...)
{
    if (!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfwprintf(g_logFile, fmt, args);
    va_end(args);
    fflush(g_logFile);
}

/* ============================================================================
 * HELPERS — Chemin du dossier DLL
 * ============================================================================ */

static void ObtenirDossierDLL(wchar_t* dossier, int taille)
{
    dossier[0] = L'\0';
    HMODULE hSelf = NULL;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&ObtenirDossierDLL, &hSelf);
    GetModuleFileNameW(hSelf, dossier, taille);
    wchar_t* lastSlash = wcsrchr(dossier, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
}

/* ============================================================================
 * LAYOUT CONFIG — layout.json (ordre gauche->droite + ecran principal)
 * ============================================================================
 *
 * Format du fichier layout.json (a cote de la DLL) :
 * {
 *   "bureau_order": ["23EA53", "LG FULL HD"],
 *   "primary_index": 0
 * }
 */

static const int MAX_BUREAU_ECRANS = 8;

struct LayoutConfig
{
    wchar_t noms[MAX_BUREAU_ECRANS][64];  // noms conviviaux, dans l'ordre gauche->droite
    int     count;                         // nombre d'ecrans dans l'ordre
    int     primaryIndex;                  // index de l'ecran principal (0-based)
    bool    valide;                        // true si le fichier a ete lu avec succes
};

static LayoutConfig LireLayoutConfig()
{
    LayoutConfig cfg = {};
    cfg.valide = false;
    cfg.primaryIndex = 0;

    wchar_t dossier[MAX_PATH];
    ObtenirDossierDLL(dossier, MAX_PATH);
    wchar_t chemin[MAX_PATH];
    _snwprintf_s(chemin, MAX_PATH, _TRUNCATE, L"%slayout.json", dossier);

    FILE* f = NULL;
    _wfopen_s(&f, chemin, L"r, ccs=UTF-8");
    if (!f) return cfg;

    // Lire tout le fichier (max 4K)
    wchar_t buf[4096] = {};
    size_t total = 0;
    while (total < 4095)
    {
        wchar_t c;
        if (fread(&c, sizeof(wchar_t), 1, f) != 1) break;
        buf[total++] = c;
    }
    fclose(f);
    buf[total] = L'\0';

    // Parser "bureau_order": [...]
    const wchar_t* arr = wcsstr(buf, L"\"bureau_order\"");
    if (!arr) return cfg;
    arr = wcschr(arr, L'[');
    if (!arr) return cfg;
    arr++; // skip '['

    const wchar_t* arrEnd = wcschr(arr, L']');
    if (!arrEnd) return cfg;

    // Extraire chaque "nom" entre guillemets
    const wchar_t* p = arr;
    while (p < arrEnd && cfg.count < MAX_BUREAU_ECRANS)
    {
        const wchar_t* q1 = wcschr(p, L'"');
        if (!q1 || q1 >= arrEnd) break;
        q1++;
        const wchar_t* q2 = wcschr(q1, L'"');
        if (!q2 || q2 >= arrEnd) break;

        int len = (int)(q2 - q1);
        if (len > 63) len = 63;
        wcsncpy_s(cfg.noms[cfg.count], 64, q1, len);
        cfg.count++;
        p = q2 + 1;
    }

    // Parser "primary_index": N
    const wchar_t* pi = wcsstr(buf, L"\"primary_index\"");
    if (pi)
    {
        pi = wcschr(pi, L':');
        if (pi)
        {
            pi++;
            while (*pi == L' ' || *pi == L'\t') pi++;
            cfg.primaryIndex = _wtoi(pi);
            if (cfg.primaryIndex < 0 || cfg.primaryIndex >= cfg.count)
                cfg.primaryIndex = 0;
        }
    }

    cfg.valide = (cfg.count > 0);
    return cfg;
}

static void EcrireLayoutConfig(const LayoutConfig& cfg)
{
    wchar_t dossier[MAX_PATH];
    ObtenirDossierDLL(dossier, MAX_PATH);
    wchar_t chemin[MAX_PATH];
    _snwprintf_s(chemin, MAX_PATH, _TRUNCATE, L"%slayout.json", dossier);

    FILE* f = NULL;
    _wfopen_s(&f, chemin, L"w, ccs=UTF-8");
    if (!f) return;

    fwprintf(f, L"{\n  \"bureau_order\": [");
    for (int i = 0; i < cfg.count; i++)
    {
        if (i > 0) fwprintf(f, L", ");
        fwprintf(f, L"\"%s\"", cfg.noms[i]);
    }
    fwprintf(f, L"],\n  \"primary_index\": %d\n}\n", cfg.primaryIndex);
    fclose(f);
}

/* ============================================================================
 * KEYWORD MATCHING (insensible a la casse)
 * ============================================================================ */

static bool ContientMotCle(const wchar_t* texte, const wchar_t* motCle)
{
    if (!texte || !motCle) return false;
    wchar_t bufTexte[512] = {};
    wchar_t bufMotCle[64] = {};
    for (int i = 0; i < 511 && texte[i]; i++)
        bufTexte[i] = towlower(texte[i]);
    for (int i = 0; i < 63 && motCle[i]; i++)
        bufMotCle[i] = towlower(motCle[i]);
    return wcsstr(bufTexte, bufMotCle) != NULL;
}

static bool ContientUnDesMotsCles(const wchar_t* texte,
                                   const wchar_t* const motsCles[])
{
    for (int i = 0; motsCles[i] != NULL; i++)
        if (ContientMotCle(texte, motsCles[i]))
            return true;
    return false;
}

/* ============================================================================
 * CCD — IDENTIFICATION DES CIBLES
 * ============================================================================ */

static bool ObtenirNomCible(
    const DISPLAYCONFIG_PATH_INFO& path,
    DISPLAYCONFIG_TARGET_DEVICE_NAME& outName)
{
    ZeroMemory(&outName, sizeof(outName));
    outName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    outName.header.size = sizeof(outName);
    outName.header.adapterId = path.targetInfo.adapterId;
    outName.header.id = path.targetInfo.id;
    return (DisplayConfigGetDeviceInfo(&outName.header) == ERROR_SUCCESS);
}

static Role DeterminerRole(const wchar_t* friendlyName,
                            const wchar_t* devicePath)
{
    if (ContientUnDesMotsCles(friendlyName, KEYWORDS_BUREAU) ||
        ContientUnDesMotsCles(devicePath, KEYWORDS_BUREAU))
        return Role::BUREAU;

    if (ContientUnDesMotsCles(friendlyName, KEYWORDS_TV) ||
        ContientUnDesMotsCles(devicePath, KEYWORDS_TV))
        return Role::TV;

    return Role::INCONNU;
}

/* ============================================================================
 * CCD — LECTURE DE LA TOPOLOGIE
 * ============================================================================ */

static bool LireTopologie(
    std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
    std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
    UINT32 numPaths = 0, numModes = 0;

    LONG result = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numPaths, &numModes);
    if (result != ERROR_SUCCESS)
    {
        Log(L"  GetDisplayConfigBufferSizes ECHEC: %d\n", result);
        return false;
    }

    paths.resize(numPaths);
    modes.resize(numModes);

    result = QueryDisplayConfig(
        QDC_ALL_PATHS,
        &numPaths, paths.data(),
        &numModes, modes.data(),
        nullptr
    );

    if (result != ERROR_SUCCESS)
    {
        Log(L"  QueryDisplayConfig ECHEC: %d\n", result);
        return false;
    }

    paths.resize(numPaths);
    modes.resize(numModes);

    Log(L"  Topologie lue: %u paths, %u modes\n", numPaths, numModes);
    return true;
}

/* ============================================================================
 * CCD — APPLICATION DE LA TOPOLOGIE
 * ============================================================================ */

static bool AppliquerTopologie(
    std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
    std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
    LONG result = SetDisplayConfig(
        (UINT32)paths.size(), paths.data(),
        (UINT32)modes.size(), modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
        SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE
    );

    Log(L"  SetDisplayConfig -> %d (%s)\n", result,
        result == ERROR_SUCCESS ? L"OK" : L"ECHEC");

    return (result == ERROR_SUCCESS);
}

/* ============================================================================
 * HELPERS — Deduplication des cibles (adapterId + target ID)
 * ============================================================================
 *
 * Avec QDC_ALL_PATHS, Windows retourne TOUS les chemins possibles
 * (source -> cible). Il peut y avoir plusieurs chemins vers la meme cible
 * physique. On ne doit activer qu'UN SEUL chemin par cible unique.
 */

static bool LuidEqual(LUID a, LUID b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

struct TargetId
{
    LUID   adapterId;
    UINT32 id;
};

static bool TargetDejaVu(const std::vector<TargetId>& liste,
                          LUID adapterId, UINT32 id)
{
    for (const auto& t : liste)
        if (LuidEqual(t.adapterId, adapterId) && t.id == id)
            return true;
    return false;
}

/* ============================================================================
 * LOGIQUE CENTRALE — BasculerMode
 * ============================================================================
 *
 * Algorithme en 3 etapes :
 *
 *   1. LIRE la topologie complete (QDC_ALL_PATHS)
 *
 *   2. MODIFIER les flags DISPLAYCONFIG_PATH_ACTIVE :
 *      - Cibles TV ou BUREAU -> activer ou desactiver selon le mode.
 *      - Cibles INCONNU (Meta VR, GPU orphelin) -> NE PAS TOUCHER.
 *      - Deduplication : un seul chemin actif par cible unique.
 *      - On prefere les chemins deja actifs (modes valides en memoire).
 *
 *   3. APPLIQUER via SetDisplayConfig avec SDC_ALLOW_CHANGES
 *      -> Windows gere coordonnees, modes et resolutions tout seul.
 *      -> Plus AUCUN conflit de coordonnees !
 */

static bool BasculerMode(bool modeTv)
{
    LogOuvrir();
    Log(L"\n========== %s ==========\n",
        modeTv ? L"ActiverModeTV()" : L"ActiverModeBureau()");

    // -- Etape 1 : Lire la topologie complete --
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;

    if (!LireTopologie(paths, modes))
    {
        LogFermer();
        return false;
    }

    // -- Etape 2 : Classifier chaque chemin --
    struct PathInfo
    {
        size_t  index;
        Role    role;
        bool    wasActive;
        LUID    adapterId;
        UINT32  targetId;
        LUID    sourceAdapterId;
        UINT32  sourceId;
        wchar_t friendlyName[64];
    };

    std::vector<PathInfo> infos;

    for (size_t i = 0; i < paths.size(); i++)
    {
        auto& path = paths[i];

        if (!path.targetInfo.targetAvailable)
            continue;

        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName;
        if (!ObtenirNomCible(path, targetName))
            continue;

        Role role = DeterminerRole(
            targetName.monitorFriendlyDeviceName,
            targetName.monitorDevicePath
        );

        bool wasActive = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;

        Log(L"  Path[%zu]: \"%s\" | %s | %s\n",
            i,
            targetName.monitorFriendlyDeviceName,
            wasActive ? L"ACTIF" : L"inactif",
            role == Role::TV     ? L"TV" :
            role == Role::BUREAU ? L"BUREAU" : L"INCONNU"
        );

        PathInfo pi = {};
        pi.index = i;
        pi.role = role;
        pi.wasActive = wasActive;
        pi.adapterId = path.targetInfo.adapterId;
        pi.targetId = path.targetInfo.id;
        pi.sourceAdapterId = path.sourceInfo.adapterId;
        pi.sourceId = path.sourceInfo.id;
        wcsncpy_s(pi.friendlyName, 64, targetName.monitorFriendlyDeviceName, 63);
        infos.push_back(pi);
    }

    // -- Etape 2a : Desactiver tous les chemins TV/BUREAU --
    // (les INCONNU gardent leur etat actuel, on ne les touche PAS)
    for (auto& info : infos)
    {
        if (info.role == Role::TV || info.role == Role::BUREAU)
            paths[info.index].flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
    }

    // -- Etape 2b : Activer les chemins voulus --
    //
    // BUREAU ETENDU (pas clone) :
    //   Deux chemins CCD qui partagent la MEME source GPU → clone (duplique).
    //   Deux chemins CCD avec des sources DIFFERENTES → extend (etendu).
    //   On doit donc s'assurer que chaque cible BUREAU utilise une source unique.
    //
    // Strategie :
    //   - Tracker les sources deja utilisees (usedSources)
    //   - Pour chaque cible, preferer un chemin dont la source est libre
    //   - En mode TV (1 seul ecran), pas de contrainte de source

    std::vector<TargetId> activated;
    std::vector<TargetId> usedSources;  // sources GPU deja assignees

    // Fonction lambda : verifier si une source est deja utilisee
    auto sourceDejaUtilisee = [&](LUID sAdapterId, UINT32 sId) -> bool {
        for (const auto& s : usedSources)
            if (LuidEqual(s.adapterId, sAdapterId) && s.id == sId)
                return true;
        return false;
    };

    // Collecter les cibles uniques a activer
    struct CibleVoulue {
        LUID    adapterId;
        UINT32  targetId;
        wchar_t friendlyName[64];
    };
    std::vector<CibleVoulue> ciblesVoulues;

    for (auto& info : infos)
    {
        bool shouldActivate = (modeTv  && info.role == Role::TV) ||
                              (!modeTv && info.role == Role::BUREAU);

        if (shouldActivate &&
            !TargetDejaVu(activated, info.adapterId, info.targetId))
        {
            CibleVoulue cv = {};
            cv.adapterId = info.adapterId;
            cv.targetId = info.targetId;
            wcsncpy_s(cv.friendlyName, 64, info.friendlyName, 63);
            ciblesVoulues.push_back(cv);
            activated.push_back({info.adapterId, info.targetId});
        }
    }

    // -- Etape 2c : Reordonner selon layout.json (mode Bureau) --
    // L'ordre dans ciblesVoulues determine la position gauche->droite.
    // Le primaryIndex determine quel ecran est a (0,0) = barre des taches.
    LayoutConfig layoutCfg = {};
    int layoutPrimaryIdx = 0;

    if (!modeTv && ciblesVoulues.size() > 1)
    {
        layoutCfg = LireLayoutConfig();

        if (layoutCfg.valide)
        {
            Log(L"  Layout config: %d ecrans, primary=%d\n",
                layoutCfg.count, layoutCfg.primaryIndex);

            // Reordonner ciblesVoulues selon l'ordre du layout
            std::vector<CibleVoulue> ordonnees;

            // D'abord, ajouter les cibles dans l'ordre du layout
            for (int li = 0; li < layoutCfg.count; li++)
            {
                for (auto& cv : ciblesVoulues)
                {
                    if (_wcsicmp(cv.friendlyName, layoutCfg.noms[li]) == 0)
                    {
                        ordonnees.push_back(cv);
                        break;
                    }
                }
            }

            // Ajouter les cibles non trouvees dans le layout (nouveaux ecrans)
            for (auto& cv : ciblesVoulues)
            {
                bool found = false;
                for (auto& o : ordonnees)
                    if (LuidEqual(o.adapterId, cv.adapterId) && o.targetId == cv.targetId)
                    { found = true; break; }
                if (!found) ordonnees.push_back(cv);
            }

            if (!ordonnees.empty())
            {
                ciblesVoulues = ordonnees;
                layoutPrimaryIdx = layoutCfg.primaryIndex;
                if (layoutPrimaryIdx < 0 || layoutPrimaryIdx >= (int)ciblesVoulues.size())
                    layoutPrimaryIdx = 0;
            }

            for (size_t ci = 0; ci < ciblesVoulues.size(); ci++)
                Log(L"  Ordre layout[%zu]: \"%s\"%s\n",
                    ci, ciblesVoulues[ci].friendlyName,
                    (int)ci == layoutPrimaryIdx ? L" (PRINCIPAL)" : L"");
        }
        else
        {
            Log(L"  Pas de layout.json -> ordre par defaut\n");
        }
    }

    // Pour chaque cible voulue, trouver le meilleur chemin
    // Priorite : (1) deja actif + source libre, (2) inactif + source libre,
    //            (3) deja actif (meme si source prise), (4) inactif
    activated.clear();

    for (auto& cible : ciblesVoulues)
    {
        size_t bestIdx = (size_t)-1;
        int    bestScore = -1;

        for (auto& info : infos)
        {
            if (!LuidEqual(info.adapterId, cible.adapterId) ||
                info.targetId != cible.targetId)
                continue;

            bool srcLibre = !sourceDejaUtilisee(info.sourceAdapterId, info.sourceId);
            int score = 0;
            if (info.wasActive && srcLibre)  score = 4;  // ideal
            else if (!info.wasActive && srcLibre) score = 3;
            else if (info.wasActive)         score = 2;  // clone fallback
            else                             score = 1;

            if (score > bestScore)
            {
                bestScore = score;
                bestIdx = info.index;
            }
        }

        if (bestIdx != (size_t)-1)
        {
            paths[bestIdx].flags |= DISPLAYCONFIG_PATH_ACTIVE;
            activated.push_back({cible.adapterId, cible.targetId});

            // Enregistrer la source utilisee
            auto& chosenPath = paths[bestIdx];
            usedSources.push_back({chosenPath.sourceInfo.adapterId,
                                    chosenPath.sourceInfo.id});

            Log(L"  -> Activer Path[%zu] (source=%u, score=%d)\n",
                bestIdx, chosenPath.sourceInfo.id, bestScore);
        }
    }

    Log(L"  Cibles activees: %zu\n", activated.size());

    if (activated.empty())
    {
        Log(L"  ERREUR: Aucune cible a activer!\n");
        LogFermer();
        return false;
    }

    // -- Etape 3 : Appliquer la nouvelle topologie --
    bool ok = AppliquerTopologie(paths, modes);

    Log(L"  Resultat: %s\n", ok ? L"OK" : L"ECHEC");
    LogFermer();
    return ok;
}

/* ============================================================================
 * FONCTIONS EXPORTEES
 * ============================================================================ */

GESTIONECRANS_API bool ActiverModeTV()
{
    return BasculerMode(true);
}

GESTIONECRANS_API bool ActiverModeBureau()
{
    return BasculerMode(false);
}

/*
 * ObtenirEcransBureau
 * Retourne les noms conviviaux des ecrans BUREAU detectes, separes par '|'.
 * Ex: "23EA53|LG FULL HD"
 * L'UI utilise cette liste pour peupler le panneau de configuration layout.
 */
GESTIONECRANS_API void ObtenirEcransBureau(wchar_t* buffer, int tailleMax)
{
    if (!buffer || tailleMax <= 0) return;
    buffer[0] = L'\0';

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!LireTopologie(paths, modes)) return;

    std::vector<TargetId> seen;
    int pos = 0;

    for (size_t i = 0; i < paths.size(); i++)
    {
        auto& path = paths[i];
        if (!path.targetInfo.targetAvailable) continue;
        if (TargetDejaVu(seen, path.targetInfo.adapterId, path.targetInfo.id))
            continue;
        seen.push_back({path.targetInfo.adapterId, path.targetInfo.id});

        DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
        if (!ObtenirNomCible(path, tn)) continue;

        Role role = DeterminerRole(tn.monitorFriendlyDeviceName, tn.monitorDevicePath);
        if (role != Role::BUREAU) continue;

        int nameLen = (int)wcslen(tn.monitorFriendlyDeviceName);
        int needed = (pos > 0) ? nameLen + 1 : nameLen; // +1 for separator
        if (pos + needed >= tailleMax - 1) break;

        if (pos > 0) { wcscat_s(buffer, tailleMax, L"|"); pos++; }
        wcscat_s(buffer, tailleMax, tn.monitorFriendlyDeviceName);
        pos += nameLen;
    }
}

/*
 * ObtenirLayoutConfig
 * Retourne la config layout actuelle au format:
 * "nom1|nom2|...\nprimary_index"
 * Si pas de layout.json, retourne une chaine vide.
 */
GESTIONECRANS_API void ObtenirLayoutConfig(wchar_t* buffer, int tailleMax)
{
    if (!buffer || tailleMax <= 0) return;
    buffer[0] = L'\0';

    LayoutConfig cfg = LireLayoutConfig();
    if (!cfg.valide) return;

    int pos = 0;
    for (int i = 0; i < cfg.count; i++)
    {
        int nameLen = (int)wcslen(cfg.noms[i]);
        int needed = (pos > 0) ? nameLen + 1 : nameLen;
        if (pos + needed >= tailleMax - 2) break;

        if (pos > 0) { wcscat_s(buffer, tailleMax, L"|"); pos++; }
        wcscat_s(buffer, tailleMax, cfg.noms[i]);
        pos += nameLen;
    }

    // Ajouter newline + primary index
    wchar_t piStr[16];
    _snwprintf_s(piStr, 16, _TRUNCATE, L"\n%d", cfg.primaryIndex);
    if (pos + (int)wcslen(piStr) < tailleMax - 1)
        wcscat_s(buffer, tailleMax, piStr);
}

/*
 * DefinirOrdreBureau
 * Recoit la config layout au format: "nom1|nom2|...\nprimary_index"
 * Sauvegarde dans layout.json.
 */
GESTIONECRANS_API void DefinirOrdreBureau(const wchar_t* config)
{
    if (!config || !config[0]) return;

    LayoutConfig cfg = {};

    // Copier pour pouvoir modifier
    wchar_t buf[2048] = {};
    wcsncpy_s(buf, 2048, config, 2047);

    // Separer la ligne des noms et le primary_index
    wchar_t* newline = wcschr(buf, L'\n');
    if (newline)
    {
        *newline = L'\0';
        cfg.primaryIndex = _wtoi(newline + 1);
    }

    // Parser les noms separes par '|'
    wchar_t* ctx = NULL;
    wchar_t* token = wcstok_s(buf, L"|", &ctx);
    while (token && cfg.count < MAX_BUREAU_ECRANS)
    {
        wcsncpy_s(cfg.noms[cfg.count], 64, token, 63);
        cfg.count++;
        token = wcstok_s(NULL, L"|", &ctx);
    }

    if (cfg.primaryIndex < 0 || cfg.primaryIndex >= cfg.count)
        cfg.primaryIndex = 0;

    cfg.valide = (cfg.count > 0);
    if (cfg.valide)
        EcrireLayoutConfig(cfg);
}

/* ============================================================================
 * DIAGNOSTIC — ObtenirInfoEcrans (affiche dans l'UI C#)
 * ============================================================================ */

static void DiagAppend(wchar_t* buffer, int tailleMax, int* pos,
                       const wchar_t* fmt, ...)
{
    if (*pos >= tailleMax - 2) return;
    wchar_t ligne[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(ligne, 511, _TRUNCATE, fmt, args);
    va_end(args);
    int len = (int)wcslen(ligne);
    if (*pos + len >= tailleMax - 1) return;
    wcscat_s(buffer, tailleMax, ligne);
    *pos += len;
}

GESTIONECRANS_API void ObtenirInfoEcrans(wchar_t* buffer, int tailleMax)
{
    if (!buffer || tailleMax <= 0) return;
    buffer[0] = L'\0';
    int pos = 0;

    UINT32 numPaths = 0, numModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numPaths, &numModes)
        != ERROR_SUCCESS)
    {
        DiagAppend(buffer, tailleMax, &pos,
            L"ERREUR: GetDisplayConfigBufferSizes\n");
        return;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);

    if (QueryDisplayConfig(QDC_ALL_PATHS,
                           &numPaths, paths.data(),
                           &numModes, modes.data(), nullptr) != ERROR_SUCCESS)
    {
        DiagAppend(buffer, tailleMax, &pos,
            L"ERREUR: QueryDisplayConfig\n");
        return;
    }

    paths.resize(numPaths);
    modes.resize(numModes);

    DiagAppend(buffer, tailleMax, &pos,
        L"=== CCD: %u paths, %u modes ===\n\n",
        numPaths, numModes);

    // Afficher chaque cible UNIQUE (dedupliquee)
    std::vector<TargetId> seen;

    for (size_t i = 0; i < paths.size(); i++)
    {
        auto& path = paths[i];

        if (!path.targetInfo.targetAvailable)
            continue;

        if (TargetDejaVu(seen, path.targetInfo.adapterId,
                         path.targetInfo.id))
            continue;
        seen.push_back({path.targetInfo.adapterId, path.targetInfo.id});

        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName;
        if (!ObtenirNomCible(path, targetName))
            continue;

        bool actif = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        Role role = DeterminerRole(
            targetName.monitorFriendlyDeviceName,
            targetName.monitorDevicePath
        );

        const wchar_t* roleStr =
            role == Role::TV     ? L"TV" :
            role == Role::BUREAU ? L"BUREAU" : L"???";

        DiagAppend(buffer, tailleMax, &pos,
            L"[%s] %s | %s\n    %s\n\n",
            actif ? L"ON " : L"OFF",
            targetName.monitorFriendlyDeviceName,
            roleStr,
            targetName.monitorDevicePath
        );
    }

    // -- Ecrire aussi en JSON (diagnostic_ecrans.json) --
    wchar_t jsonPath[MAX_PATH] = {};
    HMODULE hSelf = NULL;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&DiagAppend, &hSelf);
    GetModuleFileNameW(hSelf, jsonPath, MAX_PATH);
    wchar_t* slash = wcsrchr(jsonPath, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(jsonPath, MAX_PATH, L"diagnostic_ecrans.json");

    FILE* f = NULL;
    _wfopen_s(&f, jsonPath, L"w, ccs=UTF-8");
    if (f)
    {
        fwprintf(f, L"{\n  \"ccd_targets\": [\n");

        std::vector<TargetId> jsonSeen;
        bool first = true;

        for (size_t i = 0; i < paths.size(); i++)
        {
            auto& path = paths[i];
            if (!path.targetInfo.targetAvailable) continue;
            if (TargetDejaVu(jsonSeen,
                              path.targetInfo.adapterId,
                              path.targetInfo.id))
                continue;
            jsonSeen.push_back({path.targetInfo.adapterId,
                                 path.targetInfo.id});

            DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
            if (!ObtenirNomCible(path, tn)) continue;

            bool a = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
            Role r = DeterminerRole(
                tn.monitorFriendlyDeviceName,
                tn.monitorDevicePath);

            if (!first) fwprintf(f, L",\n");
            first = false;

            fwprintf(f, L"    {\n");
            fwprintf(f, L"      \"friendly_name\": \"%s\",\n",
                tn.monitorFriendlyDeviceName);
            fwprintf(f, L"      \"device_path\": \"%s\",\n",
                tn.monitorDevicePath);
            fwprintf(f, L"      \"role\": \"%s\",\n",
                r == Role::TV     ? L"TV" :
                r == Role::BUREAU ? L"BUREAU" : L"INCONNU");
            fwprintf(f, L"      \"active\": %s\n",
                a ? L"true" : L"false");
            fwprintf(f, L"    }");
        }

        fwprintf(f, L"\n  ]\n}\n");
        fclose(f);
    }
}

/* ============================================================================
 * POINT D'ENTREE DLL
 * ============================================================================ */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved)
{
    (void)hModule;
    (void)ul_reason_for_call;
    (void)lpReserved;
    return TRUE;
}
