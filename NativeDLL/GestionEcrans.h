#pragma once

/*
 * GestionEcrans.h
 * ---------------
 * Header pour la DLL native de gestion multi-écrans.
 * Auto-détecte les écrans via EnumDisplayDevices (EDID / nom constructeur)
 * et assigne automatiquement les rôles TV / Bureau.
 *
 * Utilisation C# :
 *   [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl)]
 *   static extern bool ActiverModeTV();
 *
 *   [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl)]
 *   static extern bool ActiverModeBureau();
 *
 *   [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl)]
 *   static extern void ObtenirInfoEcrans(
 *       [MarshalAs(UnmanagedType.LPWStr)] StringBuilder buffer, int tailleMax);
 */

#ifdef GESTIONECRANS_EXPORTS
    #define GESTIONECRANS_API extern "C" __declspec(dllexport)
#else
    #define GESTIONECRANS_API extern "C" __declspec(dllimport)
#endif

/*
 * ActiverModeTV
 * -------------
 * Auto-détecte les écrans, désactive les moniteurs de bureau (LG),
 * active la TV comme écran principal (0,0).
 */
GESTIONECRANS_API bool ActiverModeTV();

/*
 * ActiverModeBureau
 * -----------------
 * Auto-détecte les écrans, désactive la TV,
 * active le premier LG comme principal (0,0),
 * active le second LG à droite (résolution détectée automatiquement).
 */
GESTIONECRANS_API bool ActiverModeBureau();

/*
 * ObtenirInfoEcrans
 * -----------------
 * Fonction de diagnostic : remplit un buffer avec la liste des écrans
 * détectés et leurs rôles assignés. Utile pour l'affichage côté C#.
 */
GESTIONECRANS_API void ObtenirInfoEcrans(wchar_t* buffer, int tailleMax);
