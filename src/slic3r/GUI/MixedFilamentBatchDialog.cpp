#include "MixedFilamentBatchDialog.hpp"
#include "MFDBatchActiveAccordion.hpp"
#include "MFDBatchRecommendedAccordion.hpp"
#include "Widgets/FilamentCardMixed.hpp"
#include "Widgets/Button.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "I18N.hpp"
#include "MFDTheme.hpp"
#include "MixedColorMatchHelpers.hpp"

#include <wx/dcgraph.h>
#include <algorithm>
#include <cmath>

namespace Slic3r::GUI {

namespace {
bool is_gradient_def(const MixedFilamentDefinition& definition)
{
    return definition.recipe.kind == MixedFilamentRecipeKind::WeightedBlend &&
           definition.behavior.gradient.enabled && definition.recipe.blend.components.size() >= 2;
}
}

MixedFilamentBatchDialog::MixedFilamentBatchDialog(
    wxWindow* parent,
    const std::vector<std::pair<std::string, std::string>>& physical_filaments)
    : DPIDialog(parent, wxID_ANY, _L("Batch Manage Mixed Filaments"),
                wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_physical_filaments(physical_filaments)
    , m_resize_timer(this)
{
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        m_scroll_win->FitInside();
        Layout();
        Refresh(true); // force refresh parent and children background too
    }, m_resize_timer.GetId());

    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        m_resize_timer.Start(100, wxTIMER_ONE_SHOT);
        event.Skip();
    });

    std::vector<std::string> physical_colors;
    physical_colors.reserve(m_physical_filaments.size());
    for (const auto &filament : m_physical_filaments)
        physical_colors.emplace_back(filament.first);
    const MixedFilamentDisplayContext display_context = build_mixed_filament_display_context(physical_colors);
    m_physical_tds         = display_context.physical_tds;
    m_physical_material_ids = display_context.physical_material_ids;

    generate_items();
    build_ui();
}

void MixedFilamentBatchDialog::build_ui()
{
    SetBackgroundColour(MFDTheme::dialog_background());

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(main_sizer);

    // Title divider (matching MixedFilamentDialog)
    wxPanel* title_divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    title_divider->SetBackgroundColour(MFDTheme::divider());
    main_sizer->Add(title_divider, 0, wxEXPAND);

    // Scrolled window for the accordions content (styled grey)
    m_scroll_win = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL);
    m_scroll_win->SetScrollRate(0, 20);
    m_scroll_win->SetDoubleBuffered(true);
    m_scroll_win->SetBackgroundColour(MFDTheme::content_background());

    m_content_sizer = new wxBoxSizer(wxVERTICAL);

    // Active Mixed Filaments Accordion (on top)
    m_active_accordion = new MFDBatchActiveAccordion(m_scroll_win, m_mix_items);
    m_content_sizer->Add(m_active_accordion, 0, wxEXPAND | wxALL, FromDIP(8));

    // Mixing Recommendations Accordion (below it)
    m_recommended_accordion = new MFDBatchRecommendedAccordion(m_scroll_win, m_mix_items, m_physical_filaments);
    m_content_sizer->Add(m_recommended_accordion, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    m_scroll_win->SetSizer(m_content_sizer);
    m_scroll_win->Layout();

    m_recommended_accordion->wrap_explainer(m_scroll_win->GetClientSize().x);

    m_scroll_win->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        int w = m_scroll_win->GetClientSize().x;
        m_recommended_accordion->wrap_explainer(w);
        m_scroll_win->FitInside();
        m_content_sizer->Layout();
        event.Skip();
    });

    // Connect them via callbacks
    m_active_accordion->set_on_item_toggled([this]() {
        m_recommended_accordion->update_states();
        update_footer_info();
    });

    m_recommended_accordion->set_on_item_toggled([this]() {
        m_active_accordion->update_states();
        update_footer_info();
    });

    main_sizer->Add(m_scroll_win, 1, wxEXPAND);

    // --- Footer ---
    m_footer_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(30)), wxBORDER_NONE);
    m_footer_panel->SetBackgroundColour(GetBackgroundColour());

    wxBoxSizer* footer_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_footer_panel->SetSizer(footer_sizer);

    m_info_label = new wxStaticText(m_footer_panel, wxID_ANY, "");
    m_info_label->SetFont(::Label::Body_14);
    MFDTheme::apply_text(m_info_label, MFDTheme::primary_text(), m_footer_panel->GetBackgroundColour());

    Button* btn_cancel = new Button(m_footer_panel, _L("Cancel"), "", 0, 0, wxID_CANCEL);
    btn_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    Button* btn_ok = new Button(m_footer_panel, _L("OK"), "", 0, 0, wxID_OK);
    btn_ok->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });

    footer_sizer->Add(m_info_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(16));
    footer_sizer->AddStretchSpacer();
    footer_sizer->Add(btn_cancel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    footer_sizer->Add(btn_ok, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    main_sizer->Add(m_footer_panel, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));

    // Initial state refresh
    m_active_accordion->update_states();
    m_recommended_accordion->update_states();
    update_footer_info();

    // Sizes
    SetMinSize(wxSize(this->wxWindow::FromDIP(500), this->wxWindow::FromDIP(500)));
    SetSize(wxSize(this->wxWindow::FromDIP(500), this->wxWindow::FromDIP(600)));
    Layout();
    CenterOnParent();
}

void MixedFilamentBatchDialog::generate_items()
{
    m_mix_items.clear();

    auto *preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle)
        return;

    const size_t num_phys = m_physical_filaments.size();
    auto &mgr = preset_bundle->mixed_filaments;
    std::vector<MixedFilamentDefinition> existing_definitions = mgr.mixed_filament_definitions(num_phys);

    std::vector<std::string> physical_colors;
    physical_colors.reserve(m_physical_filaments.size());
    for (const auto& filament : m_physical_filaments)
        physical_colors.emplace_back(filament.first);
    const MixedFilamentDisplayContext display_context = build_mixed_filament_display_context(physical_colors);

    // Map existing recipes for lookup
    std::vector<std::pair<BatchMixKey, size_t>> existing_keys;
    size_t visible_index = 0;
    for (size_t idx = 0; idx < existing_definitions.size(); ++idx) {
        if (!existing_definitions[idx].visibility.tombstoned) {
            existing_keys.push_back({make_mix_key(existing_definitions[idx]), visible_index++});
        }
    }

    auto register_item = [&](const std::vector<int>& phys, const std::vector<int>& weights, bool recommended) {
        BatchMixKey key = make_mix_key(phys, weights);

        // Find if this is already registered in m_mix_items
        auto it = std::find_if(m_mix_items.begin(), m_mix_items.end(), [&](const BatchMixItem& item) {
            return item.key == key;
        });

        if (it != m_mix_items.end()) {
            if (recommended)
                it->is_recommended = true;
            return;
        }

        BatchMixItem item;
        item.key = key;
        item.physical_indices = phys;
        item.percentages = weights;
        item.color = compute_mixed_color(phys, weights);
        item.tooltip = format_tooltip(phys, weights, item.color);
        item.is_recommended = recommended;

        // Check if existing
        auto exist_it = std::find_if(existing_keys.begin(), existing_keys.end(), [&](const std::pair<BatchMixKey, size_t>& k) {
            return k.first == key;
        });
        if (exist_it != existing_keys.end()) {
            item.is_existing = true;
            item.is_deleted = false;
            item.display_id = wxString(Slic3r::mixed_filament_index_to_letter(exist_it->second));
        }

        m_mix_items.push_back(item);
    };

    // 1. First register all non-tombstoned existing mixes
    for (size_t idx = 0; idx < existing_definitions.size(); ++idx) {
        auto& def = existing_definitions[idx];
        if (def.visibility.tombstoned)
            continue;

        BatchMixKey key = make_mix_key(def);
        BatchMixItem item;
        item.key = key;
        item.stable_id = def.identity.stable_id;
        item.is_existing = true;
        item.is_deleted = false;
        item.is_recommended = false;

        auto exist_it = std::find_if(existing_keys.begin(), existing_keys.end(), [&](const std::pair<BatchMixKey, size_t>& k) {
            return k.first == key;
        });
        if (exist_it != existing_keys.end()) {
            item.display_id = wxString(Slic3r::mixed_filament_index_to_letter(exist_it->second));
        }

        if (def.behavior.surface_bias.perimeter_modulation) {
            item.is_apm = true;
            item.is_gradient = true;
            for (const auto& comp : def.recipe.blend.components) {
                item.physical_indices.push_back((int)comp.filament.id - 1);
                item.percentages.push_back(comp.percent);
            }
            std::vector<unsigned int> component_ids = def.recipe.blend.component_ids(num_phys);
            const MixedFilamentGradientPreview preview = build_mixed_filament_gradient_preview(component_ids, {}, display_context);
            item.gradient_colors = preview.sampled_colors;
            if (item.gradient_colors.empty()) {
                for (int p : item.physical_indices) {
                    if (p >= 0 && p < int(m_physical_filaments.size()))
                        item.gradient_colors.push_back(wxColor(m_physical_filaments[p].first));
                }
            }
            if (item.gradient_colors.empty()) {
                item.gradient_colors = {StateColor::darkModeColorFor(wxColour("#808080")),
                                        StateColor::darkModeColorFor(wxColour("#B0B0B0"))};
            }
            if (!item.gradient_colors.empty())
                item.color = item.gradient_colors.front();

            wxString apm_prefix = "Adaptive Perimeter Modulation";
            for (int p : item.physical_indices) {
                apm_prefix += wxString::Format(" [%d]", p + 1);
            }
            item.tooltip = apm_prefix;

            m_mix_items.push_back(item);
        } else if (is_gradient_def(def)) {
            item.is_gradient = true;
            for (const auto& comp : def.recipe.blend.components) {
                item.physical_indices.push_back((int)comp.filament.id - 1);
                item.percentages.push_back(comp.percent);
            }
            const MixedFilamentGradientPreview preview = build_mixed_filament_gradient_preview(def, display_context);
            item.gradient_colors = preview.sampled_colors;
            if (item.gradient_colors.empty()) {
                std::vector<unsigned int> component_ids = def.recipe.blend.component_ids(num_phys);
                item.gradient_colors = build_mixed_filament_gradient_preview(component_ids, {}, display_context).sampled_colors;
            }
            if (item.gradient_colors.empty()) {
                for (int p : item.physical_indices) {
                    if (p >= 0 && p < int(m_physical_filaments.size()))
                        item.gradient_colors.push_back(wxColor(m_physical_filaments[p].first));
                }
            }
            if (!item.gradient_colors.empty())
                item.color = item.gradient_colors.front();

            ColorNames::DescriptionOptions options;
            options.include_components = true;
            options.include_hex        = true;
            item.tooltip = wxString::FromUTF8(
                ColorNames::mixed_filament_name(def, display_context.physical_material_types, display_context.physical_colors, options));

            m_mix_items.push_back(item);
        } else if (def.recipe.kind == MixedFilamentRecipeKind::ManualPattern) {
            item.is_pattern = true;
            if (def.recipe.manual_pattern && !def.recipe.manual_pattern->groups.empty()) {
                for (const auto& g : def.recipe.manual_pattern->groups[0]) {
                    item.physical_indices.push_back((int)g.id - 1);
                }
            }
            std::string hex = compute_mixed_filament_display_color(def, display_context);
            item.color = parse_mixed_color(hex);

            ColorNames::DescriptionOptions options;
            options.include_components = true;
            options.include_hex        = true;
            item.tooltip = wxString::FromUTF8(
                ColorNames::mixed_filament_name(def, display_context.physical_material_types, display_context.physical_colors, options));

            m_mix_items.push_back(item);
        } else {
            for (const auto& comp : def.recipe.blend.components) {
                item.physical_indices.push_back((int)comp.filament.id - 1);
                item.percentages.push_back(comp.percent);
            }
            item.color = compute_mixed_color(item.physical_indices, item.percentages);
            item.tooltip = format_tooltip(item.physical_indices, item.percentages, item.color);

            m_mix_items.push_back(item);
        }
    }

    // 2. Generate recommended combinations
    // 2-Way 50/50
    for (int i = 0; i < num_phys; ++i) {
        for (int j = i + 1; j < num_phys; ++j) {
            register_item({i, j}, {50, 50}, true);
        }
    }

    // 2-Way 34/66
    for (int i = 0; i < num_phys; ++i) {
        for (int j = 0; j < num_phys; ++j) {
            if (i == j) continue;
            register_item({i, j}, {34, 66}, true);
        }
    }

    // 3-Way 33/33/34
    for (int i = 0; i < num_phys; ++i) {
        for (int j = i + 1; j < num_phys; ++j) {
            for (int k = j + 1; k < num_phys; ++k) {
                register_item({i, j, k}, {33, 33, 34}, true);
            }
        }
    }
}

void MixedFilamentBatchDialog::update_footer_info()
{
    int add_count = 0;
    int delete_count = 0;

    const auto& phys_enabled = m_recommended_accordion->get_physical_enabled();

    for (const auto& item : m_mix_items) {
        if (item.is_recommended && !item.is_existing) {
            bool is_shown = true;
            for (int idx : item.physical_indices) {
                if (idx >= 0 && idx < phys_enabled.size() && !phys_enabled[idx]) {
                    is_shown = false;
                    break;
                }
            }
            if (is_shown && item.is_added) {
                add_count++;
            }
        }
        if (item.is_existing && item.is_deleted) {
            delete_count++;
        }
    }

    wxString info_text;
    if (add_count > 0 && delete_count > 0) {
        info_text = wxString::Format("+%d -%d", add_count, delete_count);
    } else if (add_count > 0) {
        info_text = wxString::Format("+%d", add_count);
    } else if (delete_count > 0) {
        info_text = wxString::Format("-%d", delete_count);
    } else {
        info_text = "";
    }

    m_info_label->SetLabel(info_text);
    m_footer_panel->Layout();
}

BatchMixKey MixedFilamentBatchDialog::make_mix_key(const std::vector<int>& physical_indices, const std::vector<int>& percentages) const
{
    BatchMixKey key;
    key.kind = BatchItemKind::Blend;
    for (size_t idx = 0; idx < physical_indices.size(); ++idx) {
        key.components.push_back({physical_indices[idx] + 1, percentages[idx]});
    }
    std::sort(key.components.begin(), key.components.end());
    return key;
}

BatchMixKey MixedFilamentBatchDialog::make_mix_key(const MixedFilamentDefinition& def) const
{
    BatchMixKey key;
    key.stable_id = def.identity.stable_id;

    if (def.behavior.surface_bias.perimeter_modulation) {
        key.kind = BatchItemKind::APM;
        for (const auto& comp : def.recipe.blend.components) {
            key.components.push_back({(int)comp.filament.id, comp.percent});
        }
    } else if (is_gradient_def(def)) {
        key.kind = BatchItemKind::Gradient;
        for (const auto& comp : def.recipe.blend.components) {
            key.components.push_back({(int)comp.filament.id, comp.percent});
        }
    } else if (def.recipe.kind == MixedFilamentRecipeKind::ManualPattern) {
        key.kind = BatchItemKind::Pattern;
        if (def.recipe.manual_pattern && !def.recipe.manual_pattern->groups.empty()) {
            for (const auto& group : def.recipe.manual_pattern->groups[0]) {
                key.components.push_back({(int)group.id, 0});
            }
        }
    } else {
        key.kind = BatchItemKind::Blend;
        for (const auto& comp : def.recipe.blend.components) {
            key.components.push_back({(int)comp.filament.id, comp.percent});
        }
    }

    std::sort(key.components.begin(), key.components.end());
    return key;
}

wxColor MixedFilamentBatchDialog::compute_mixed_color(const std::vector<int>& physical_indices, const std::vector<int>& percentages) const
{
    std::vector<MixedFilamentColorInput> inputs;
    const size_t count = std::min(physical_indices.size(), percentages.size());
    inputs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const int index = physical_indices[i];
        if (index < 0 || index >= int(m_physical_filaments.size()) || percentages[i] <= 0)
            continue;

        std::optional<double> td_mm;
        if (index < int(m_physical_tds.size())) {
            const double value = m_physical_tds[size_t(index)];
            if (std::isfinite(value) && value > 0.0)
                td_mm = value;
        }
        std::optional<std::string> material_id;
        if (index < int(m_physical_material_ids.size()) && !m_physical_material_ids[size_t(index)].empty())
            material_id = m_physical_material_ids[size_t(index)];

        MixedFilamentColorInput input;
        input.color_hex  = m_physical_filaments[size_t(index)].first;
        input.percent    = percentages[i];
        input.td_mm      = td_mm;
        input.material_id = material_id;
        inputs.emplace_back(std::move(input));
    }

    const wxColor color(MixedFilamentManager::blend_color_multi(inputs));
    return color.IsOk() ? color : wxColor("#26A69A");
}

wxString MixedFilamentBatchDialog::format_tooltip(const std::vector<int>& physical_indices, const std::vector<int>& percentages, const wxColor& mixed_color) const
{
    std::vector<std::string> physical_colors;
    physical_colors.reserve(m_physical_filaments.size());
    for (const auto& f : m_physical_filaments)
        physical_colors.emplace_back(f.first);
    const MixedFilamentDisplayContext ctx = build_mixed_filament_display_context(physical_colors);

    char hex_buf[16];
    std::snprintf(hex_buf, sizeof(hex_buf), "#%02X%02X%02X", mixed_color.Red(), mixed_color.Green(), mixed_color.Blue());

    ColorNames::DescriptionOptions opts;
    opts.include_components = true;

    std::string tip = ColorNames::mixed_filament_name(physical_indices, percentages, std::string(hex_buf),
                                                    ctx.physical_material_types, ctx.physical_colors, opts);
    return from_u8(tip);
}

void MixedFilamentBatchDialog::apply_batch_changes()
{
    auto *preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle)
        return;

    auto &mgr = preset_bundle->mixed_filaments;
    const size_t num_physical = m_physical_filaments.size();

    ConfigOptionStrings *color_opt = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    std::vector<std::string> physical_colors = color_opt ? color_opt->values : std::vector<std::string>();
    physical_colors.resize(num_physical, "#26A69A");

    std::vector<MixedFilamentDefinition> definitions = mgr.mixed_filament_definitions(num_physical);
    std::vector<MixedFilamentDefinition> old_mixed = definitions;
    auto canonical_pair = [](unsigned int a, unsigned int b) {
        return std::make_pair(std::min(a, b), std::max(a, b));
    };

    // 1. Handle Deletions
    for (const auto& item : m_mix_items) {
        if (item.is_existing && item.is_deleted && !item.is_apm) {
            auto it = std::find_if(definitions.begin(), definitions.end(), [&](const MixedFilamentDefinition& d) {
                if (d.behavior.surface_bias.perimeter_modulation)
                    return false;
                if (item.stable_id != 0 && d.identity.stable_id == item.stable_id)
                    return true;
                return make_mix_key(d) == item.key;
            });
            if (it != definitions.end()) {
                size_t mixed_id = std::distance(definitions.begin(), it);

                auto& target = definitions[mixed_id];
                const MixedFilamentPrimaryPairView target_pair_view = target.recipe.blend.primary_pair_or();
                const std::pair<unsigned int, unsigned int> target_pair = canonical_pair(target_pair_view.component_a.id, target_pair_view.component_b.id);
                const bool valid_auto_pair = target.recipe.kind == MixedFilamentRecipeKind::WeightedBlend &&
                                             !target.behavior.gradient.enabled &&
                                             target_pair.first >= 1 &&
                                             target_pair.second >= 1 &&
                                             target_pair.first <= num_physical &&
                                             target_pair.second <= num_physical &&
                                             target_pair.first != target_pair.second;

                if (target.source.kind == MixedFilamentSourceKind::Custom && target.source.origin_auto && valid_auto_pair) {
                    bool tombstoned_existing_auto = false;
                    for (size_t idx = 0; idx < definitions.size(); ++idx) {
                        if (idx == mixed_id)
                            continue;
                        MixedFilamentDefinition &candidate = definitions[idx];
                        if (candidate.source.kind == MixedFilamentSourceKind::Custom)
                            continue;
                        const MixedFilamentPrimaryPairView candidate_pair = candidate.recipe.blend.primary_pair_or();
                        if (canonical_pair(candidate_pair.component_a.id, candidate_pair.component_b.id) != target_pair)
                            continue;
                        candidate.visibility.tombstoned = true;
                        tombstoned_existing_auto = true;
                        break;
                    }

                    if (tombstoned_existing_auto) {
                        definitions.erase(definitions.begin() + ptrdiff_t(mixed_id));
                    } else {
                        target.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
                        target.recipe.blend.components = {
                            {MixedFilamentPhysicalRef{target_pair.first}, 50},
                            {MixedFilamentPhysicalRef{target_pair.second}, 50}
                        };
                        target.recipe.manual_pattern.reset();
                        target.behavior.layer_cadence.component_a_layers = 1;
                        target.behavior.layer_cadence.component_b_layers = 1;
                        target.behavior.distribution = MixedFilamentDistributionMode::Simple;
                        target.source.kind = MixedFilamentSourceKind::AutoGenerated;
                        target.source.origin_auto = true;
                        target.visibility.tombstoned = true;
                    }
                } else if (target.source.kind == MixedFilamentSourceKind::Custom) {
                    definitions.erase(definitions.begin() + ptrdiff_t(mixed_id));
                } else {
                    target.visibility.tombstoned = true;
                }
            }
        }
    }

    mgr.set_mixed_filament_definitions(definitions, physical_colors);

    // 2. Handle Additions
    const auto& phys_enabled = m_recommended_accordion->get_physical_enabled();
    for (const auto& item : m_mix_items) {
        bool is_shown = true;
        for (int idx : item.physical_indices) {
            if (idx >= 0 && idx < (int)phys_enabled.size() && !phys_enabled[idx]) {
                is_shown = false;
                break;
            }
        }
        if (item.is_recommended && !item.is_existing && item.is_added && is_shown) {
            MixedFilamentDefinition definition;
            definition.source.kind = MixedFilamentSourceKind::Custom;
            definition.source.origin_auto = false;
            definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;

            for (size_t idx = 0; idx < item.physical_indices.size(); ++idx) {
                unsigned int phys_id = static_cast<unsigned int>(item.physical_indices[idx] + 1);
                int percent = item.percentages[idx];
                definition.recipe.blend.components.push_back({{phys_id}, percent});
            }

            mgr.add_custom_filament_definition(definition, physical_colors);
        }
    }

    // 3. Persist and Sync
    const std::string serialized = mgr.serialize_custom_entries();
    if (ConfigOptionString *opt = preset_bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
        opt->value = serialized;
    else
        preset_bundle->project_config.set_key_value("mixed_filament_definitions", new ConfigOptionString(serialized));

    wxGetApp().plater()->sidebar().update_mixed_filament_panel(false);
    preset_bundle->update_mixed_filament_id_remap(old_mixed, num_physical, num_physical);

    if (auto* print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT))
        print_tab->update_dirty();
    if (wxGetApp().mainframe)
        wxGetApp().mainframe->on_config_changed(&preset_bundle->project_config);

    wxGetApp().plater()->update_project_dirty_from_presets();
    wxGetApp().plater()->on_filaments_change(num_physical);
}

void MixedFilamentBatchDialog::on_dpi_changed(const wxRect&)
{
    SetMinSize(wxSize(this->wxWindow::FromDIP(500), this->wxWindow::FromDIP(500)));
    Refresh();
}

// ==========================================
// BatchSwatchTile Implementation
// ==========================================

BatchSwatchTile::BatchSwatchTile(wxWindow* parent, BatchMixItem* item, std::function<void()> on_toggled)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(28), parent->FromDIP(28)), wxBORDER_NONE)
    , m_item(item)
    , m_on_toggled(std::move(on_toggled))
{
    SetMinSize(wxSize(FromDIP(28), FromDIP(28)));
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(m_item->is_apm ? wxCursor(wxNullCursor) : wxCursor(wxCURSOR_HAND));
    SetDoubleBuffered(true);
    SetToolTip(m_item->tooltip);

    Bind(wxEVT_PAINT, &BatchSwatchTile::on_paint, this);
    Bind(wxEVT_ENTER_WINDOW, &BatchSwatchTile::on_enter, this);
    Bind(wxEVT_LEAVE_WINDOW, &BatchSwatchTile::on_leave, this);
    Bind(wxEVT_LEFT_UP, &BatchSwatchTile::on_left_up, this);
}

void BatchSwatchTile::on_paint(wxPaintEvent&)
{
    wxPaintDC pdc(this);
    wxGCDC dc(pdc);
    wxSize s = GetClientSize();
    int padding = m_hovered ? 0 : FromDIP(2);

    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();

    wxString text = (m_item->is_existing && !m_item->is_deleted) ? m_item->display_id : "";

    if (m_item->is_gradient) {
        FilamentCardMixed::paint_clr_swatch_gradient(dc, s, m_item->gradient_colors, text, wxGetApp().dark_mode(), padding);
    } else {
        wxColor color = m_item->color;
        FilamentCardMixed::paint_clr_swatch(dc, s, color, text, wxGetApp().dark_mode(), padding);
    }

    // Antialiased overlays
    wxColor text_col = m_item->color.GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE;
    dc.SetPen(wxPen(text_col, FromDIP(2)));
    if (m_item->is_existing) {
        if (m_item->is_deleted && !m_item->is_apm) {
            dc.DrawLine(s.x * 0.3, s.y * 0.3, s.x * 0.7, s.y * 0.7);
        }
    } else {
        if (m_item->is_added) {
            int cx = s.x / 2;
            int cy = s.y / 2;
            int r = s.x / 4;
            dc.DrawLine(cx - r, cy, cx + r, cy);
            dc.DrawLine(cx, cy - r, cx, cy + r);
        }
    }
}

void BatchSwatchTile::on_enter(wxMouseEvent& event)
{
    if (!m_item->is_apm)
        m_hovered = true;
    Refresh();
    event.Skip();
}

void BatchSwatchTile::on_leave(wxMouseEvent& event)
{
    m_hovered = false;
    Refresh();
    event.Skip();
}

void BatchSwatchTile::on_left_up(wxMouseEvent& event)
{
    if (m_item->is_apm) {
        event.Skip();
        return;
    }
    if (m_item->is_existing) {
        m_item->is_deleted = !m_item->is_deleted;
    } else {
        m_item->is_added = !m_item->is_added;
    }
    Refresh();
    if (m_on_toggled) {
        m_on_toggled();
    }
    event.Skip();
}


// ==========================================
// PhysicalFilamentTile Implementation
// ==========================================

PhysicalFilamentTile::PhysicalFilamentTile(wxWindow* parent, size_t index, const std::string& color_hex, const std::string& name, std::vector<bool>& enabled_ref, std::function<void()> on_toggled)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(28), parent->FromDIP(28)), wxBORDER_NONE)
    , m_index(index)
    , m_color_hex(color_hex)
    , m_name(name)
    , m_enabled_ref(enabled_ref)
    , m_on_toggled(std::move(on_toggled))
{
    SetMinSize(wxSize(FromDIP(28), FromDIP(28)));
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_HAND));
    SetDoubleBuffered(true);
    update_tooltip();

    Bind(wxEVT_PAINT, &PhysicalFilamentTile::on_paint, this);
    Bind(wxEVT_ENTER_WINDOW, &PhysicalFilamentTile::on_enter, this);
    Bind(wxEVT_LEAVE_WINDOW, &PhysicalFilamentTile::on_leave, this);
    Bind(wxEVT_LEFT_UP, &PhysicalFilamentTile::on_left_up, this);
}

void PhysicalFilamentTile::update_tooltip()
{
    wxString tooltip = m_enabled_ref[m_index] ? "Disable - " : "Enable - ";
    tooltip += wxString::Format(" [%d] %s", (int)(m_index + 1), m_name);
    SetToolTip(tooltip);
}

void PhysicalFilamentTile::on_paint(wxPaintEvent&)
{
    wxPaintDC pdc(this);
    wxGCDC dc(pdc);
    wxSize s = GetClientSize();
    bool enabled = m_enabled_ref[m_index];
    wxColor bg_color = GetParent()->GetBackgroundColour();

    dc.SetBackground(wxBrush(bg_color));
    dc.Clear();

    wxColor color(m_color_hex);
    wxString text = enabled ? wxString::Format("%d", (int)(m_index + 1)) : "";

    if (!enabled) {
        color = wxColor(
            (color.Red() + bg_color.Red()) / 2,
            (color.Green() + bg_color.Green()) / 2,
            (color.Blue() + bg_color.Blue()) / 2
        );
    }

    int padding = m_hovered ? 0 : FromDIP(2);
    FilamentCardMixed::paint_clr_swatch(dc, s, color, text, wxGetApp().dark_mode(), padding);
}

void PhysicalFilamentTile::on_enter(wxMouseEvent& event)
{
    m_hovered = true;
    Refresh();
    event.Skip();
}

void PhysicalFilamentTile::on_leave(wxMouseEvent& event)
{
    m_hovered = false;
    Refresh();
    event.Skip();
}

void PhysicalFilamentTile::on_left_up(wxMouseEvent& event)
{
    m_enabled_ref[m_index] = !m_enabled_ref[m_index];
    update_tooltip();
    Refresh();
    if (m_on_toggled) {
        m_on_toggled();
    }
    event.Skip();
}


// ==========================================
// BatchCheckBox Implementation
// ==========================================

BatchCheckBox::BatchCheckBox(wxWindow* parent, std::function<void()> on_clicked)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(16), parent->FromDIP(16)), wxBORDER_NONE)
    , m_on_clicked(std::move(on_clicked))
{
    SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_HAND));
    SetDoubleBuffered(true);

    Bind(wxEVT_PAINT, &BatchCheckBox::on_paint, this);
    Bind(wxEVT_ENTER_WINDOW, &BatchCheckBox::on_enter, this);
    Bind(wxEVT_LEAVE_WINDOW, &BatchCheckBox::on_leave, this);
    Bind(wxEVT_LEFT_UP, &BatchCheckBox::on_left_up, this);
}

void BatchCheckBox::set_state(int state)
{
    m_state = state;
    Refresh();
}

void BatchCheckBox::on_paint(wxPaintEvent&)
{
    wxPaintDC pdc(this);
    wxGCDC dc(pdc);
    wxSize s = GetClientSize();

    dc.SetBackground(wxBrush(GetParent() ? GetParent()->GetBackgroundColour() : StateColor::darkModeColorFor(*wxWHITE)));
    dc.Clear();

    // Checkbox border (adaptive color on dark mode and hover)
    wxColor border_color;
    if (wxGetApp().dark_mode()) {
        border_color = m_hovered ? *wxWHITE : wxColor(80, 80, 80);
    } else {
        border_color = m_hovered ? wxColor(26, 26, 26) : wxColor(204, 204, 204);
    }

    dc.SetPen(wxPen(border_color, FromDIP(1)));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, s.x, s.y, FromDIP(2));

    if (m_state == 1) {
        // State 1: Box
        dc.SetBrush(wxBrush(wxColour("#009688")));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRoundedRectangle(s.x * 0.25, s.y * 0.25, s.x * 0.5, s.y * 0.5, FromDIP(1));
    } else if (m_state == 2) {
        // State 2: Checked
        dc.SetPen(wxPen(wxColour("#009688"), FromDIP(2)));
        dc.DrawLine(s.x * 0.25, s.y * 0.55, s.x * 0.45, s.y * 0.75);
        dc.DrawLine(s.x * 0.45, s.y * 0.75, s.x * 0.75, s.y * 0.35);
    }
}

void BatchCheckBox::on_enter(wxMouseEvent& event)
{
    m_hovered = true;
    Refresh();
    event.Skip();
}

void BatchCheckBox::on_leave(wxMouseEvent& event)
{
    m_hovered = false;
    Refresh();
    event.Skip();
}

void BatchCheckBox::on_left_up(wxMouseEvent& event)
{
    if (m_on_clicked) {
        m_on_clicked();
    }
    event.Skip();
}

} // namespace Slic3r::GUI
