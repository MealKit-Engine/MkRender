#include "Strings.h"

#include <mkcontract/User.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <unordered_map>

namespace mkrender::tool {
namespace {

struct Pair {
    const char* en;
    const char* ko;
};

// When new text is shown on screen, add its Korean here: mkrender_strings_test fails on tr("...") text without a line.
// Korean follows engine docs/TOOL_DESIGN.md section 4: short, plain words, sentences in 해요체, buttons and menu items a verb noun
constexpr Pair kKorean[] = {
    {"Autosaved to %s", "%s에 자동 저장했어요"},
    {"Terrain brush", "지형 브러시"},
    {"Scatter brush", "흩뿌리기 브러시"},
    {"Open a project to paint (files go to its assets/terrain).", "칠하려면 프로젝트를 여세요 (파일은 프로젝트의 assets/terrain에 저장돼요)."},
    {"New flat heightmap", "새 평평한 높이맵"},
    {"New instance file", "새 배치 파일"},
    {"%s (written when the scene is saved)", "%s (씬을 저장할 때 써요)"},
    {"Cannot read %s", "%s 파일을 읽을 수 없어요"},
    {"%u x %u samples, %.2f m cells", "샘플 %u x %u개, 칸 %.2f m"},
    {"%zu instances", "배치 %zu개"},
    {"Brush (B)", "브러시 (B)"},
    {"Clear instances", "배치 모두 지우기"},
    {"Raise", "올리기"},
    {"Lower", "내리기"},
    {"Flatten", "평평하게"},
    {"Smooth", "부드럽게"},
    {"Paint", "칠하기"},
    {"Erase", "지우기"},
    {"Radius (m)", "반경 (m)"},
    {"Strength", "세기"},
    {"Left drag paints; Flatten keeps the height under the stroke start; Ctrl+Z undoes a stroke", "왼쪽 드래그로 칠해요. 평평하게는 처음 누른 곳의 높이를 유지하고, Ctrl+Z로 한 번 그은 것을 되돌려요"},
    {"Left drag paints onto terrain and floors (Shift erases); density and sizes are fields below", "왼쪽 드래그로 지형과 바닥에 칠해요 (Shift는 지우기). 밀도와 크기는 아래 필드에서 정해요"},
    {"Editor", "에디터"},
    {"Open a project: MkRender looks for its running MealKit editor", "프로젝트를 여세요. MkRender가 그 프로젝트의 실행 중인 MealKit 에디터를 찾아요"},
    {"No MealKit editor running for this project", "이 프로젝트를 연 MealKit 에디터가 없어요"},
    {"Editor (pid %u): %s, %s%s", "에디터 (pid %u): %s, %s%s"},
    {"playing", "플레이 중"},
    {"editing", "편집 중"},
    {"no scene", "씬 없음"},
    {" (unsaved there)", " (에디터에서 저장 안 됨)"},
    {"Its game code isn't loaded yet (build it in the editor)", "게임 코드가 아직 로드되지 않았어요 (에디터에서 빌드하세요)"},
    {"Play in editor", "에디터에서 플레이"},
    {"Play from here", "여기서부터 플레이"},
    {"Solo play from this camera (or from the selected spawn point)", "이 카메라에서 혼자 플레이 (또는 선택한 스폰 지점에서)"},
    {"Stop", "정지"},
    {"Look through selected camera", "선택한 카메라로 보기"},
    {"cutscene name", "컷신 이름"},
    {"Preview cutscene", "컷신 미리보기"},
    {"End preview", "미리보기 끝내기"},
    {"The editor plays and previews the saved scene file; MkRender saves first.", "에디터는 저장된 씬 파일을 플레이하고 미리 봐요. MkRender가 먼저 저장해요."},
    {"File", "파일"},
    {"New scene", "새 씬"},
    {"Open project...", "프로젝트 열기..."},
    {"Open scene...", "씬 열기..."},
    {"Recent scenes", "최근 씬"},
    {"Project scenes", "프로젝트 씬"},
    {"Save", "저장"},
    {"Save as...", "다른 이름으로 저장..."},
    {"Reload from disk", "디스크에서 다시 불러오기"},
    {"Recover autosave", "자동 저장 복구"},
    {"Replace the scene with its last autosave (build/editor/autosave); undo goes back", "씬을 마지막 자동 저장(build/editor/autosave)으로 바꿔요. 실행 취소로 되돌릴 수 있어요"},
    {"Reload models", "모델 다시 불러오기"},
    {"Save screenshot (3D only)", "스크린샷 저장 (3D만)"},
    {"Quit", "끝내기"},
    {"Edit", "편집"},
    {"Undo", "실행 취소"},
    {"Redo", "다시 실행"},
    {"Add", "추가"},
    {"Duplicate", "복제"},
    {"Delete", "삭제"},
    {"Add here", "여기에 추가"},
    {"Open Scene", "씬 열기"},
    {"Open project", "프로젝트 열기"},
    {"No objects in this scene yet.", "이 씬에는 아직 오브젝트가 없어요."},
    {"Open a scene to edit it, or add an object to start a new one.", "씬을 열어 편집하거나, 오브젝트를 추가해 새 씬을 시작하세요."},
    {"This scene has no objects yet. Add one, or drag a model in from Assets.", "이 씬에는 아직 오브젝트가 없어요. 추가하거나 에셋에서 모델을 끌어다 놓으세요."},
    {"Select all", "모두 선택"},
    {"Arrange", "정렬"},
    {"Align min %s", "%s 최소에 맞추기"},
    {"Align center %s", "%s 가운데 맞추기"},
    {"Align max %s", "%s 최대에 맞추기"},
    {"Distribute %s", "%s 고르게 배치"},
    {"Aligns to the last selected object", "마지막에 선택한 오브젝트에 맞춰요"},
    {"Drop to floor", "바닥에 내려놓기"},
    {"Hide", "숨기기"},
    {"Show all", "모두 보이기"},
    {"Lock", "잠그기"},
    {"Unlock all", "모두 잠금 해제"},
    {"Save selection as prefab...", "선택을 프리팹으로 저장..."},
    {"View", "보기"},
    {"View Mode", "보기 모드"},
    {"Lit", "조명"},
    {"Wireframe", "와이어프레임"},
    {"Normals", "법선"},
    {"LOD Level", "LOD 단계"},
    {"Overdraw", "오버드로"},
    {"Over Budget", "예산 초과"},
    {"Material Features", "머티리얼 기능"},
    {"Color is the surface direction: red X, green Y, blue Z", "색이 표면 방향이에요: 빨강 X, 초록 Y, 파랑 Z"},
    {"LOD 0 green, 1 yellow, 2 red", "LOD 0 초록, 1 노랑, 2 빨강"},
    {"Brighter where the same pixel is drawn more times", "같은 픽셀을 여러 번 그릴수록 밝아요"},
    {"Red: over the model budget of mealkit.project", "빨강: mealkit.project의 모델 예산을 넘었어요"},
    {"Outline red, wind green, rim blue, dissolve yellow, none gray", "외곽선 빨강, 바람 초록, 림 파랑, 디졸브 노랑, 없음 회색"},
    {"MkRender draws each model's full mesh: LOD levels show in the MealKit editor", "MkRender는 모델 전체 메시만 그려요. LOD 단계는 MealKit 에디터에서 보세요"},
    {"Frame selection", "선택 화면에 맞추기"},
    {"Move", "이동"},
    {"Rotate", "회전"},
    {"Scale", "크기"},
    {"Local axes", "로컬 축"},
    {"Markers and areas", "마커와 영역"},
    {"Grid", "격자"},
    {"Grid and snap", "격자와 스냅"},
    {"Always snap (Ctrl turns it off)", "항상 스냅 (Ctrl로 끄기)"},
    {"Grid step (m)", "격자 간격 (m)"},
    {"Move step (m)", "이동 단위 (m)"},
    {"Rotate step (deg)", "회전 단위 (도)"},
    {"Scale step", "크기 단위"},
    {"Autosave (min, 0 off)", "자동 저장 (분, 0은 끔)"},
    {"Camera bookmarks", "카메라 북마크"},
    {"Go to %d", "%d번으로 가기"},
    {"Lighting model", "조명 모델"},
    {"Bake reflection probes", "반사 프로브 굽기"},
    {"Grading capture", "그레이딩 캡처"},
    {"Compress model textures (BC7/BC5)", "모델 텍스처 압축 (BC7/BC5)"},
    {"Uses the engine's texture cache (build/cache/textures) and fills it when missing. Off: plain RGBA8", "엔진의 텍스처 캐시(build/cache/textures)를 쓰고 없으면 채워요. 끄면 그냥 RGBA8이에요"},
    {"Play animations", "애니메이션 재생"},
    {"Skinned models loop the clip of their anim field (off: the clip's first frame plus its offset)", "스키닝 모델이 anim 필드의 클립을 반복해요 (끄면 클립의 첫 프레임과 오프셋)"},
    {"Reset layout", "레이아웃 초기화"},
    {"   The file changed on disk.", "   디스크의 파일이 바뀌었어요."},
    {"Reload (drop my edits)", "다시 불러오기 (내 편집 버리기)"},
    {"Keep mine", "내 것 유지"},
    {"Unsaved changes", "저장하지 않은 변경"},
    {"%s has unsaved changes.", "%s에 저장하지 않은 변경이 있어요."},
    {"The new scene", "새 씬"},
    {"Don't save", "저장 안 함"},
    {"Cancel", "취소"},
    {"Game", "게임"},
    {"  (hidden)", "  (숨김)"},
    {"  (locked)", "  (잠김)"},
    {"Show", "보이기"},
    {"Unlock", "잠금 해제"},
    {"Lock (not picked in the view)", "잠그기 (뷰에서 선택 안 됨)"},
    {"Move to top level", "최상위로 옮기기"},
    {"Frame", "화면에 맞추기"},
    {"Language", "언어"},
    {"Add object", "오브젝트 추가"},
    {"Search", "검색"},
    {", view only", ", 화면 전용"},
    {"Remove this field line", "이 필드 줄 지우기"},
    {"Add %d missing fields with defaults", "빠진 필드 %d개를 기본값으로 추가"},
    {"Add field", "필드 추가"},
    {"field name", "필드 이름"},
    {"Select an object in the hierarchy or the viewport.", "계층이나 뷰포트에서 오브젝트를 고르세요."},
    {"%zu objects in the scene", "씬의 오브젝트 %zu개"},
    {"%zu objects selected (editing the last one; the gizmo moves all)", "오브젝트 %zu개 선택됨 (마지막 것을 편집하고, 기즈모는 모두 움직여요)"},
    {"id %u", "id %u"},
    {"Name", "이름"},
    {"Kind", "종류"},
    {"Game kind (typed name):", "게임 종류 (직접 입력):"},
    {"Unknown kind: not in the project's object kinds; fields are kept as text.", "알 수 없는 종류예요. 프로젝트의 오브젝트 종류에 없어서 필드를 글자 그대로 둬요."},
    {"Game kind: fields are kept as text (no build/editor/kinds.txt yet: open the project in the editor once, or run <game>_headless --write-kinds).", "게임 종류예요. 필드를 글자 그대로 둬요 (build/editor/kinds.txt가 아직 없어요. 에디터에서 프로젝트를 한 번 열거나 <game>_headless --write-kinds를 실행하세요)."},
    {"Parent", "부모"},
    {"Unparent", "부모 해제"},
    {"Folder", "폴더"},
    {"Transform (parent-relative)", "트랜스폼 (부모 기준)"},
    {"Position", "위치"},
    {"Rotation", "회전"},
    {"Look", "모양"},
    {"Model", "모델"},
    {"(box)", "(상자)"},
    {"%u triangles, %u materials, %u textures", "삼각형 %u개, 머티리얼 %u개, 텍스처 %u개"},
    {"Color", "색"},
    {"Checker", "체커"},
    {"Clip", "클립"},
    {"(rest pose)", "(기본 자세)"},
    {"The anim field (\"<clip> [offset seconds]\"), played in a loop like in the game view", "anim 필드 (\"<클립> [오프셋 초]\")예요. 게임 화면처럼 반복 재생해요"},
    {"The heightmap fills the object's box (scale).", "높이맵이 오브젝트의 상자(크기)를 채워요."},
    {"Bake this probe", "이 프로브 굽기"},
    {"not baked", "굽지 않음"},
    {"box = influence area", "상자 = 영향 범위"},
    {"Fields", "필드"},
    {"Sun", "태양"},
    {"Azimuth", "방위각"},
    {"%.1f deg", "%.1f도"},
    {"Elevation", "고도"},
    {"Sun color", "태양 색"},
    {"Sky and fog (linear)", "하늘과 안개 (선형)"},
    {"Sky", "하늘"},
    {"Ground", "지면"},
    {"Fog color", "안개 색"},
    {"Fog density", "안개 밀도"},
    {"(none)", "(없음)"},
    {"Sky HDRI", "하늘 HDRI"},
    {"HDRI file", "HDRI 파일"},
    {"Intensity", "세기"},
    {"Not loaded (see the log): the sky/ground colors are shown.", "불러오지 못했어요 (로그 참고). 하늘과 지면 색을 대신 보여 줘요."},
    {"Baked once into build/cache/environments (the engine reads the same file).", "build/cache/environments에 한 번 구워요 (엔진도 같은 파일을 읽어요)."},
    {"Reflection probes", "반사 프로브"},
    {"%u in the scene, %u baked, %u drawn", "씬에 %u개, 구운 것 %u개, 그린 것 %u개"},
    {"Bake probes", "프로브 굽기"},
    {"%u left", "%u개 남음"},
    {"Post-processing", "후처리"},
    {"Tone mapping", "톤 매핑"},
    {"Punchy look", "선명한 느낌"},
    {"Exposure", "노출"},
    {"Bloom", "블룸"},
    {"Bloom threshold", "블룸 기준값"},
    {"Bloom tint", "블룸 색조"},
    {"Vignette", "비네트"},
    {"Vignette start", "비네트 시작"},
    {"Film grain", "필름 그레인"},
    {"Chromatic aberr.", "색수차"},
    {"Color LUT", "색 LUT"},
    {"The view without LUT to art/grading, to grade in an image editor", "LUT 없는 화면을 art/grading에 저장해요. 이미지 편집기에서 그레이딩할 때 써요"},
    {"Saves the view without the LUT to art/grading/<scene>.png with a neutral.cube beside it:\ngrade both in Resolve or Photoshop, export a .cube into assets/lut and pick it here.", "LUT 없는 화면을 art/grading/<씬>.png로, 옆에 neutral.cube와 함께 저장해요.\nResolve나 Photoshop에서 둘 다 그레이딩하고 .cube를 assets/lut로 내보낸 뒤 여기서 고르세요."},
    {"%.0f fps, %u draw calls, %u instances", "%.0f fps, 드로우 콜 %u개, 인스턴스 %u개"},
    {"GPU %.2f ms (opaque %.2f, shadow %.2f, post %.2f)", "GPU %.2f ms (불투명 %.2f, 그림자 %.2f, 후처리 %.2f)"},
    {"VSync", "수직 동기화"},
    {"Log", "로그"},
    {"Save prefab", "프리팹 저장"},
    {"Saves the %zu selected objects (with their children) to assets/prefabs/<name>.prefab", "선택한 오브젝트 %zu개(자식 포함)를 assets/prefabs/<이름>.prefab에 저장해요"},
    {"Open a project to see its assets.", "에셋을 보려면 프로젝트를 여세요."},
    {"Rescan", "다시 찾기"},
    {"Pictures", "그림"},
    {"Search assets", "에셋 검색"},
    {"Place %s", "%s 놓기"},
    {"Place at the view center", "화면 가운데에 놓기"},
    {"Find uses in this scene", "이 씬에서 쓰인 곳 찾기"},
    {"Copy path", "경로 복사"},
    {"Models", "모델"},
    {"Drag into the view to place, double-click places at the view center", "뷰로 끌어 놓거나, 더블클릭하면 화면 가운데에 놓아요"},
    {"Prefabs", "프리팹"},
    {"Placed as plain copies (Edit > Save selection as prefab makes one)", "그냥 복사본으로 놓여요 (편집 > 선택을 프리팹으로 저장으로 만들어요)"},
    {"Images", "이미지"},
    {"Placed as sprites", "스프라이트로 놓여요"},
    {"Scenes", "씬"},
    {"Double-click opens", "더블클릭하면 열어요"},
    {"Hierarchy", "계층"},
    {"Inspector", "인스펙터"},
    {"Environment", "환경"},
    {"Stats", "통계"},
    {"Assets", "에셋"},
    {"Look Dev", "룩데브"},
    // Sprites
    {"Sprite", "스프라이트"},
    {"Image", "이미지"},
    {"Pick a PNG here, or drag one from Assets into the view.", "여기서 PNG를 고르거나, 에셋에서 뷰로 끌어다 놓으세요."},
    {"Columns, rows", "열, 행"},
    {"%u x %u pixels, one cell %u x %u", "%u x %u 픽셀, 한 칸 %u x %u"},
    {"Cell %d (column %d, row %d)", "칸 %d (%d열, %d행)"},
    {"Pivot", "피벗"},
    {"Top left", "왼쪽 위"},
    {"Top", "위"},
    {"Top right", "오른쪽 위"},
    {"Left", "왼쪽"},
    {"Center", "가운데"},
    {"Right", "오른쪽"},
    {"Bottom left", "왼쪽 아래"},
    {"Bottom", "아래"},
    {"Bottom right", "오른쪽 아래"},
    // Hierarchy problems
    {"Model %s: %s", "모델 %s: %s"},
    {"No image", "이미지 없음"},
    {"Image %s: %s", "이미지 %s: %s"},
    {"Unknown kind %s: not in the project's object kinds", "알 수 없는 종류 %s: 프로젝트의 오브젝트 종류에 없어요"},
    {"Reflection probe not baked", "반사 프로브를 아직 굽지 않았어요"},
    // Look Dev
    {"Light", "조명"},
    {"Scene setting", "씬 설정"},
    {"Turn", "회전"},
    {"Zoom", "확대"},
    {"Flash", "번쩍임"},
    {"Dissolve", "디졸브"},
    {"Click a model in Assets > Models, or select an object with a model.", "에셋 > 모델에서 모델을 누르거나, 모델이 있는 오브젝트를 선택하세요."},
    {"Cannot load %s", "%s을(를) 불러올 수 없어요"},
    {"Neutral studio", "중립 스튜디오"},
    {"Daylight", "한낮"},
    {"Sunset", "노을"},
    {"Night", "밤"},
};

bool g_korean = true;

const std::unordered_map<std::string_view, const char*>& koreanMap() {
    static const std::unordered_map<std::string_view, const char*> map = [] {
        std::unordered_map<std::string_view, const char*> m;
        for (const Pair& p : kKorean) m.emplace(p.en, p.ko);
        return m;
    }();
    return map;
}

std::filesystem::file_time_type g_settingsTime{};
std::chrono::steady_clock::time_point g_nextCheck{};

std::filesystem::file_time_type settingsTime() {
    std::error_code ec;
    const std::string path = mkcontract::userPath(mkcontract::kEditorSettings);
    if (path.empty()) return {};
    const auto t = std::filesystem::last_write_time(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

} // namespace

void setLanguage(const std::string& code) { g_korean = code != "en"; }

const char* language() { return g_korean ? "ko" : "en"; }

const char* tr(const char* english) {
    if (!g_korean) return english;
    const auto& map = koreanMap();
    const auto it = map.find(english);
    return it == map.end() ? english : it->second;
}

std::string trf(const char* english, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, english);
    std::vsnprintf(buffer, sizeof(buffer), tr(english), args);
    va_end(args);
    return buffer;
}

bool hasTranslation(const char* english) { return koreanMap().count(english) != 0; }

void loadLanguage() {
    const std::string code = mkcontract::editorSetting("language");
    setLanguage(code.empty() ? "ko" : code);
    g_settingsTime = settingsTime();
}

bool saveLanguage(const std::string& code) {
    if (!mkcontract::setEditorSetting("language", code)) return false;
    setLanguage(code);
    g_settingsTime = settingsTime();
    return true;
}

bool followLanguage() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextCheck) return false;
    g_nextCheck = now + std::chrono::seconds(1);
    const auto t = settingsTime();
    if (t == g_settingsTime) return false;
    const std::string before = language();
    loadLanguage();
    return before != language();
}

} // namespace mkrender::tool
